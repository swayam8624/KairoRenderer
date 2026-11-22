# Vulkan PBR descriptor rebuild: validation, GPU lifetime and exception safety

Continue from [the texture upload and sampled-image guide](TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md), [Vulkan frame failure recovery](VULKAN_FRAME_FAILURE_RECOVERY_LAB.md), and the concrete [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm) implementation. This laboratory traces the historical input-validation and partial-cleanup fixes, **followed by the now-completed transactional generation refactor**. Read the newer [first-principles generation laboratory](TRANSACTIONAL_VULKAN_DESCRIPTOR_GENERATIONS_LAB.md) alongside this chapter for the full strong-guarantee implementation.

**Evidence:** the source path and ownership rules were inspected and modified. Native Vulkan allocation-failure injection, driver validation, and a full C++23 superbuild have not been executed during this pass.

## 1. What a descriptor set owns, and what it borrows

A descriptor set is not the texture or uniform buffer. It stores GPU-visible *binding information* that points to resources owned elsewhere. The PBR shader layout has ten bindings:

| Slot | Descriptor type | Borrowed object |
| --- | --- | --- |
| 0 | UBO | frame camera/light uniform |
| 1 | combined image/sampler | directional depth shadow |
| 2–6 | combined image/sampler | base color, normal, metallic-roughness, emissive, occlusion |
| 7 | UBO | per-draw PBR material factors |
| 8 | combined image/sampler | environment |
| 9 | UBO | optional per-draw copied skin palette |

The `VulkanMaterialDescriptors` object owns the stable `VkDescriptorSetLayout`, the current `VkDescriptorPool`, the allocated `VkDescriptorSet` handles and the per-draw `VulkanHostBuffer` owners for material/skin data. It **borrows** the camera buffer, shadow view/sampler, texture views/samplers and the source `SkinPalette` data pointer only during rebuilding.

`vkUpdateDescriptorSets` consumes the temporary `VkDescriptorImageInfo`, `VkDescriptorBufferInfo` and `VkWriteDescriptorSet` structures during the call. Their stack storage does not need to outlive `Rebuild`, but the actual GPU resources referred to by those structs must outlive submitted commands that use the sets.

## 2. The historical failure path

The previous `Rebuild` immediately called `ClearPool()` and only then validated the new camera/shadow bindings and each new material's factor ranges, five sampled image/sampler slots, environment and optional skin palette.

For an invalid *new* material, the algorithm could throw **after having already destroyed a perfectly good previous descriptor generation**. Subsequent attempts could reconstruct the sets, but the invariant “invalid input cannot destroy the old valid state” was broken. A later failure after pool creation could also leave partially allocated uniforms/set storage in the object's maps until a future call or destructor cleaned it.

This problem is distinct from whether the frame fence was signaled: even with every GPU command retired, a bad input should fail **before** damaging a valid old CPU/native resource generation.

## 3. Input preflight is now a separate phase

`VulkanMaterialDescriptors::Rebuild` validates and builds a **separate candidate generation** without releasing the old pool:

```text
DescriptorGeneration staged(device)      // initially empty, RAII owned
BuildAndCommitResourceGeneration(active, staged, [&](candidate) {
    if zero draws: return             // stage an EMPTY replacement

    verify cameraRange > 0 and valid shadow view/sampler
    verify numberOfDraws <= UINT32_MAX / 7
    for every draw:
        PBRMaterial.Validate()
        validate five texture view/sampler pairs
        validate environment view/sampler
        validate optional nonempty skin palette

    allocate candidate descriptor pool/sets
    allocate/copy candidate material and optional skin UBOs
    vkUpdateDescriptorSets(candidate.Sets[i], ...)
});
// If any step threw, staged destroys only its partially built resources.
// If all succeeded, active.Swap(staged) published the new generation.
// staged now owns the old generation, which retires at the fenced boundary.
```

The `UINT32_MAX / 7` guard is important even for an absurdly large scene: the pool requests `count*7` combined image/sampler descriptors. Casting an arbitrary `std::span::size()` to `uint32_t` and then multiplying without checking could wrap the requested count, making the pool insufficient for the real number of draws.

Each material and skin palette is now validated **once**, before any native descriptor pool mutation, avoiding an unnecessary second validation loop during upload.

## 4. Native construction failures now roll back a detached generation

The **earlier correction** achieved basic exception safety: it caught errors during pool/UBO creation and cleared partially built objects, but had already destroyed the old pool. The **current implementation** goes further. Every pool, set and per-draw uniform buffer belongs to a distinct `DescriptorGeneration` owner. The builder only writes to the candidate. If any validation or native creation step throws, C++ stack unwinding destroys that candidate (including its partial per-draw buffers and `VkDescriptorPool`) without touching the previously published generation.

Once all candidate sets have been allocated and updated, `BuildAndCommitResourceGeneration` invokes `active.Swap(staged)`. The swap is constrained by a compile-time `noexcept` check. The caller can therefore make the complete replacement observable without a new allocation and without leaving a half-published set array. The staged object then owns and retires the former generation.

This provides the **strong host exception guarantee** for validation and ordinary allocation exceptions: the prior active descriptor pool/set/UBO generation remains unchanged if building the replacement fails. It does *not* rescue a lost Vulkan device or guarantee that borrowed image views remain valid if other code destroys them.

**GPU retirement is still mandatory.** Both success and rollback operate under the caller's completed-frame-fence precondition. C++ exception safety does not make it legal to destroy a descriptor pool while pending GPU work is using its sets. See the [transactional-generation deep lab](TRANSACTIONAL_VULKAN_DESCRIPTOR_GENERATIONS_LAB.md).

## 5. How this interacts with KairoRenderer's WSI recovery

[`VulkanTriangle::Record`](../VulkanTriangle.cppm) calls `RebuildMaterialDescriptors()` **before** beginning the reusable command buffer. It also updates camera uniforms and CPU debug buffers. If descriptor preflight or allocation throws after the swapchain image was acquired, `VulkanRendererRuntime::DrawFrame` now:

- Has *not yet reset* the frame fence (reset moved into the Submit callback).
- Records that the ImageAvailable binary semaphore is still unconsumed.
- Attempts a synchronization-only queue wait on that semaphore, idles the GPU and rebuilds the swapchain/sync generation.
- Propagates the original descriptor exception to the host.
- Refuses later frames if native recovery itself fails.

This separation matters: the descriptor module is responsible for its own partial allocation cleanup, while the renderer facade is responsible for WSI state and the next frame's usable semaphore/fence generation. One cannot safely replace the other.

## 6. Native regression cases still required

The following are concrete targets for injected Vulkan errors, rather than assertions already proven by the CPU journal:

1. Submit a valid material scene, retire its frame, then submit an invalid material with one null sampler. `Rebuild` must reject it **without destroying the old descriptor pool**; verify using wrapper/validation instrumentation.
2. Submit a skin palette with an out-of-range joint matrix count or invalid float. The preflight should fail before native allocation.
3. Force `vkCreateDescriptorPool` failure after valid preflight; verify **the old active pool survives unchanged**, while the failed candidate has no leaked resources.
4. Force the Nth `VulkanHostBuffer` allocation to fail. Check that all earlier owned per-draw UBOs and the newly allocated pool are released.
5. Force `vkAllocateDescriptorSets` failure. Check that **the candidate generation's destructor** releases the new pool and leaves the previous active sets untouched.
6. Change material draw count, transparency, texture IDs, skin palettes, and viewport sizes across frames. Verify the renderer never binds a descriptor set from an obsolete pool while GPU work is pending.
7. Confirm uniform ABI/descriptor slot layout against the actual compiled GLSL/SPIR-V pipeline; CPU descriptor ordering alone cannot prove shader compatibility.

Run native validation layers on a real Vulkan adapter, record the exact code commit, device/driver, failure injection point, VkResult and observed allocations/destructions.

## 7. Completed improvement: a transactional descriptor generation

The planned design is now present in [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm). It constructs a detached `DescriptorGeneration` RAII owner for the new pool, sets and material/skin UBOs, validates and fills the candidate, then performs the non-throwing swap. The old generation is destroyed only after the new one is published and the previously submitted GPU frame has retired. The [portable transactional regression](../tests/TransactionalResourceGenerationTests.cpp) fault-injects validation and allocation exceptions and verifies the original active resources survive.

The next **not-yet-implemented** extension is *deferred generation retirement* for two or more frames in flight. The current one-frame path relies on its frame fence. A multiframe renderer would need a real generation ID, an in-flight completion/timeline value for each old pool, and a garbage-collection queue that retires each old generation only after all GPU submissions referencing it finish. The complete worked derivation is in [the transactional descriptor generation lab](TRANSACTIONAL_VULKAN_DESCRIPTOR_GENERATIONS_LAB.md).

Native fault injection and validation-layer execution remain outstanding; the CPU regression verifies the same host transaction helper but does not exercise native Vulkan allocation or GPU queue completion.
