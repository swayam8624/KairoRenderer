# Build a new GPU world before retiring the old one

*A first-principles laboratory on C++23 exception safety, Vulkan descriptor pools, and transactional GPU resource generations.*

Imagine editing a material in a live scene. You change its normal map, enable skinning, and ask the editor to render the next frame. Before that new frame can use the material, the renderer must create another set of shader bindings and upload the new numbers to GPU-visible uniform buffers. What happens if the eighth allocation fails?

A naïve renderer tears down the old bindings first and then starts building new ones. After that eighth failure, it has neither the completed new material nor its previously valid binding generation. A transactional renderer does the opposite: **prepare the replacement, test every part, publish it all at once, and only then retire the old state**.

This is the logic now implemented in [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm), through the independent [`BuildAndCommitResourceGeneration`](../detail/TransactionalResourceGeneration.hpp) helper. The same helper is exercised in [`TransactionalResourceGenerationTests.cpp`](../tests/TransactionalResourceGenerationTests.cpp) with injected exceptions, without requiring Vulkan headers or a GPU.

**Evidence boundary:** The source change and CPU-only test are committed. A successful native Vulkan descriptor allocation/validation-layer test on the final repository head has not yet been demonstrated. The design still requires a fence-retired frame boundary before releasing a previously bound native descriptor pool.

## 1. There are three different notions of ownership

The word *material* often hides three quite different objects.

A KairoAssets `MaterialArtifactData` is a portable CPU value: it says how much base color, roughness, emissive light or alpha the material should have and which authored `TextureAssetHandle` identifies each texture. It is useful long after a graphics device shuts down.

KairoRenderer's `PBRMaterial` and `VulkanMaterialImages` are the next step. They carry validated values and sampled image/sampler **handles** for one live renderer instance. The image/sampler handles are **borrowed** from texture owners. The temporary `SkinPalette*` is also borrowed only while building a descriptor generation; the renderer copies its joint matrices into a device-visible uniform buffer.

Finally, the Vulkan descriptor generation actually owns `VkDescriptorPool`, `VkDescriptorSet` handles allocated from that pool, and per-draw uniform buffer RAII objects. It does **not** own the camera buffer, shadow image, material texture image views, material samplers or environment image.

```text
Stable authored MaterialAssetHandle
     |
     v
CPU PBRMaterial + sampled image references
     |                           | borrow, do not destroy
     |                           v
     |                      VkImageView / VkSampler
     v
VulkanMaterialDescriptors::Rebuild
     |
     +---- ACTIVE GENERATION A  [pool A, sets A, per-draw UBOs A]
     |
     +---- STAGED GENERATION B  [pool B, sets B, per-draw UBOs B]
               |
               | all allocations & descriptor writes succeed?
               v
          noexcept Swap(A, B)
               |
               +---- ACTIVE = B (new shader bindings)
               +---- STAGED = A (former GPU objects)
                                    |
                                    v
                        staged destructor retires A
```

Two entire ownership generations temporarily coexist. This is **deliberate**: the extra transient memory is what makes rollback possible.

## 2. The old failure, with real bytecode-level consequences

Before this refactor, `Rebuild` called `ClearPool()`, then `vkCreateDescriptorPool`, `vkAllocateDescriptorSets`, and allocated one or two `VulkanHostBuffer` objects per draw. An exception while constructing the third draw's uniform buffer meant the old pool was already destroyed. The previous follow-up fixed the **basic guarantee**—partial *new* objects were cleaned—but not the **strong guarantee**, because there was no old generation to return to.

In C++ terms, these are different contracts:

| Exception guarantee | What happens on an exception? |
| --- | --- |
| No guarantee | Leaks, dangling handles, corrupted class invariants or undefined behavior are possible |
| Basic | All owners remain valid and leak-free, but old resources or prior values may be gone |
| Strong | The externally observable active state is unchanged, as if the failed operation had never published |
| No-throw | The operation itself is guaranteed not to throw |

A Vulkan driver can still fail catastrophically (such as `VK_ERROR_DEVICE_LOST`). A C++ strong guarantee for *host ownership* cannot revive a lost graphics device; the guarantee here is about an otherwise valid device and errors during candidate construction, not all possible physical-device failures.

## 3. The independent generation owner

The new nested `DescriptorGeneration` is responsible for one complete pool and its accompanying buffers. It stores:

```cpp
VkDevice Device;                    // borrowed, must remain alive
VkDescriptorPool Pool;             // owned, initially VK_NULL_HANDLE
std::vector<VkDescriptorSet> Sets; // allocated from Pool, no independent free
std::vector<std::unique_ptr<VulkanHostBuffer>> MaterialBuffers;
std::vector<std::unique_ptr<VulkanHostBuffer>> SkinBuffers;
```

Its `Clear()` destroys buffers and its descriptor pool, then makes its handle null. Its destructor calls `Clear()`; therefore a partially constructed candidate **automatically** releases every successfully acquired native object on exception unwinding. Its copy constructor and assignment are deleted because copying an owning `VkDescriptorPool` handle would cause double-destruction.

What makes the publication step safe is its explicit `void Swap(DescriptorGeneration& other) noexcept`. It swaps only a numeric pool handle and standard-allocator vectors. There is no native Vulkan call, file I/O, shader compilation or new heap allocation during publication. Both generations must borrow the same live Vulkan device.

A single shared descriptor-set layout still belongs to the parent `VulkanMaterialDescriptors` instance. Both candidate and current sets use **that same layout**, so swapping generations does not rewrite the shader's ten-binding ABI.

## 4. What the transaction helper enforces

The production helper in [`detail/TransactionalResourceGeneration.hpp`](../detail/TransactionalResourceGeneration.hpp) is intentionally tiny:

```cpp
template<class Generation, class Build>
void BuildAndCommitResourceGeneration(
    Generation& active, Generation& staged, Build&& build)
{
    static_assert(noexcept(std::declval<Generation&>().Swap(
        std::declval<Generation&>())));

    if (std::addressof(active) == std::addressof(staged))
        throw std::invalid_argument("Cannot stage onto the active owner.");

    std::invoke(std::forward<Build>(build), staged); // may throw
    active.Swap(staged);                             // must not throw
}
```

In the repository source the error message is more descriptive, but the actual steps are these. This helper does not invent a native handle or promise GPU completion. It enforces the **ordering** and checks at compile time that the final swap cannot throw.

Three possible outcomes are worth tracing by hand:

**Case A — bad texture view.** The input preflight finds a null normal-map sampler. The builder throws before creating a pool. `active` still contains the old sets. `staged` has no native resources; its destructor is trivial.

**Case B — device-local memory exhaustion.** The builder successfully creates a candidate pool, its sets and the first six uniform buffers, then `VulkanHostBuffer` throws. The stack unwinds: the seventh temporary buffer cleans itself if partially allocated; `staged` destroys all six committed candidate buffers and the candidate descriptor pool. The old active pool was never swapped or destroyed.

**Case C — complete candidate.** All bindings validate, Vulkan allocates the candidate pool and sets, `vkUpdateDescriptorSets` stores the new bindings, and the per-draw buffer vectors own their allocations. The helper performs a no-throw `Swap`. `active` now owns the fully prepared new generation; `staged` owns the old one. As the enclosing `Rebuild` returns, `staged` retires the old objects.

## 5. The ten shader bindings still mean exactly the same thing

This change is about **resource lifetime**, not lighting or shader math. Shader-visible binding indices and descriptor types are unchanged.

| Binding | Meaning | Resource owner |
| --- | --- | --- |
| 0 | Camera/frame uniform | frame resources, borrowed by set |
| 1 | Directional shadow sampled depth | shadow renderer, borrowed |
| 2 | Base-color texture | texture backend, borrowed |
| 3 | Normal texture | texture backend, borrowed |
| 4 | Metallic/roughness texture | texture backend, borrowed |
| 5 | Emissive texture | texture backend, borrowed |
| 6 | Occlusion texture | texture backend, borrowed |
| 7 | PBR factors UBO | **this generation** |
| 8 | Environment sampled texture | environment/texture backend, borrowed |
| 9 | Optional skin palette UBO | **this generation** |

The code validates every material factor, all five image views and samplers, environment view/sampler and nonempty skin palette **before** allocating a candidate pool. It also bounds the material count before the `count * 7` combined-sampler descriptor allocation could overflow `uint32_t`.

The temporary `VkWriteDescriptorSet` and `VkDescriptorImageInfo` arrays are safe stack values, because `vkUpdateDescriptorSets` copies their descriptor contents during the call. The images, samplers and UBOs they refer to are *not* automatically copied into the set; their native lifetime remains an independent requirement.

## 6. A tiny worked example with integers instead of GPU objects

Imagine the current visible scene has two draws:

```text
Generation A:
    Descriptor set handles: [10, 20]
    Uniform buffer handles: [100, 200]
```

You update the scene to three draws. A candidate begins:

```text
Generation B:
    Descriptor set handles: [30, 40, 50]
    Uniform buffer handles: [300, 400, 500]
```

Now suppose the upload for the third buffer fails. The **old code** had already erased set handles `[10,20]`; the new code destroys only the incomplete candidate and preserves the old `[10,20]` ready for the next recovery attempt.

If all three uploads succeed, the final no-throw swap makes `[30,40,50]` active and moves `[10,20]` into the outgoing generation. The outgoing object then releases `[100,200]` and its pool at the fence-safe boundary.

Note that a frame with *zero* draws is a valid transaction too. It stages an empty generation and publishes it; the previously used descriptor pool is then retired **intentionally**, not left as a ghost material cache.

## 7. The regression exercises the real transaction policy

The new [`TransactionalResourceGenerationTests.cpp`](../tests/TransactionalResourceGenerationTests.cpp) uses a `FakeGeneration` with the **same `Swap` contract**. It covers:

1. A preflight validation error with no candidate allocation: prior active generation unchanged.
2. A failure after two candidate resource allocations: the staged owner destroys just those objects; the previous live generation survives.
3. Successful replacement of a two-handle generation with a three-handle generation: old resources retire **after** publication.
4. Publishing an empty generation: all previous resources retire correctly, and the active set is empty.
5. Rejecting a candidate that aliases the active generation, preventing an invalid self-transaction.

This is not just a test of a separately invented model: `VulkanDescriptor.cppm` calls the exact same `BuildAndCommitResourceGeneration` template. The test is portable because the template depends only on host ownership and `Swap`, not on Vulkan driver calls.

The source is registered as `KairoRendererTransactionalGenerationTests` in [`CMakeLists.txt`](../CMakeLists.txt). The [GCC/Clang portable CI workflow](../.github/workflows/cpu-render-resource-guards.yml) compiles it alongside native-publication and Vulkan frame-journal regressions. The [KairoGameEngine standalone regression script](https://github.com/swayam8624/KairoGameEngine/blob/main/scripts/run_renderer_resource_lifetime_regressions.sh) includes it as the **fifth** CPU-only check.

## 8. Native GPU verification still matters

A CPU exception-safety test cannot prove a driver accepts descriptor writes, that the shader expects the exact buffer offsets, or that no command buffer still uses the old descriptor pool. A native validation suite should create real valid descriptors, render one frame, retire its fence, and then inject failures after:

- `vkCreateDescriptorPool` succeeds but `vkAllocateDescriptorSets` fails.
- The Nth `VulkanHostBuffer` allocation fails after earlier per-draw buffers are populated.
- A candidate skin palette buffer fails while ordinary material UBOs were already allocated.
- Two different scenes request different material/draw counts and descriptor environments.
- An empty scene publishes an empty generation, followed by a nonempty scene that reconstructs the sets.
- A deliberately invalid sampled-image binding is supplied while old sets still exist.

Instrument `vkDestroyDescriptorPool` and native buffer/memory destruction. At each injected failure, assert the old active pool and `Set(index)` handles are unchanged, all candidate native handles are retired, and a later successful rebuild can proceed. Repeat under validation layers with exact GPU, driver, loader, repository SHA and compiler recorded.

**Important restriction:** the strong guarantee applies to the descriptor *owner's current pool/set/UBO generation*. It does not preserve the validity of borrowed textures if some other part of the renderer destroys an old VkImageView before the new generation is published. Such resource replacements require their own generation and fence-retirement rules.

## 9. A deeper extension: multiple in-flight frames

The current backend is intentionally one frame in flight, so the previous submission's completion fence is the retirement gate. With two or three in-flight frames, swapping and immediately destroying the outgoing generation is **not** generally legal: another GPU queue submission might still reference it.

A scalable renderer should attach a monotonically increasing submission timeline value to each retired descriptor generation:

```text
new DescriptorGeneration B is published at CPU frame N
        |
        +-- old DescriptorGeneration A moves to RetiredGenerations
                                         |
                                         +-- last GPU submission using A
                                             signaled fence/timeline >= value
                                         |
                                         +-- THEN destroy A's buffers/pool
```

This connects C++ transaction theory to Vulkan scheduling: strong exception safety decides **which state is active**, while fences or timeline semaphores decide **when the old state's native memory can be reclaimed**. They solve different problems.

A future editor hot-reload workflow can reuse the same mechanism for textures, meshes, pipelines and viewport image views, provided it distinguishes authored stable `AssetID` values from process-local native resource handles.

---

**Suggested next reading:** [Vulkan frame failure recovery](VULKAN_FRAME_FAILURE_RECOVERY_LAB.md) to understand why a descriptor exception after acquiring a swapchain image also requires WSI semaphore retirement. Both the descriptor transaction and frame recovery must succeed for the next frame to render safely.
