# Vulkan PBR descriptor rebuild: validation, GPU lifetime and exception safety

Continue from [the texture upload and sampled-image guide](TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md), [Vulkan frame failure recovery](VULKAN_FRAME_FAILURE_RECOVERY_LAB.md), and the concrete [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm) implementation. This laboratory accompanies a functional input-validation/cleanup change to the PBR descriptor rebuild path.

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

`VulkanMaterialDescriptors::Rebuild` now checks before releasing the old pool:

```text
if zero draws:
    ClearPool; return  // intended disappearance of current draw sets

verify cameraRange > 0, shadowView and shadowSampler are valid
verify numberOfDraws <= UINT32_MAX / 7
for every draw:
    PBRMaterial.Validate()
    each of five image views and samplers is non-null
    environment image view and sampler are non-null
    optional non-empty skin palette validates

// Only after the entire input set passes:
ClearPool()
allocate new descriptor pool
allocate one descriptor set per draw
allocate and write material UBOs
optionally allocate/copy skin UBOs
vkUpdateDescriptorSets each draw's ten-slot shader interface
```

The `UINT32_MAX / 7` guard is important even for an absurdly large scene: the pool requests `count*7` combined image/sampler descriptors. Casting an arbitrary `std::span::size()` to `uint32_t` and then multiplying without checking could wrap the requested count, making the pool insufficient for the real number of draws.

Each material and skin palette is now validated **once**, before any native descriptor pool mutation, avoiding an unnecessary second validation loop during upload.

## 4. Native construction failures now clear partial state immediately

After the old pool is released, native creation and per-draw UBO allocation happen inside a `try` block. On any exception, `ClearPool()`:

1. Releases skin UBO owners, followed by material UBO owners.
2. Clears the current descriptor-set handle vector.
3. Destroys the `VkDescriptorPool` if present, making all its sets invalid.
4. Resets the pool handle to `VK_NULL_HANDLE`.
5. Rethrows the original exception so the renderer's frame-state handler can retire/rebuild the acquired WSI frame if needed.

This is **basic exception safety** for mid-rebuild allocations: after failure, the descriptor object holds no partially constructed pool/UBOs. It is **not** strong exception safety. The previous valid pool was already destroyed after successful preflight, so an out-of-device-memory or `std::bad_alloc` during native construction still leaves the descriptor generation empty until a later successful rebuild.

Also note the caller's frame fence must be complete before `ClearPool()`. Destroying a descriptor pool whose sets are still bound by pending GPU commands is not made legal by catch/cleanup logic.

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
3. Force `vkCreateDescriptorPool` failure after valid preflight, then verify old pool is destroyed as specified, no partial UBOs remain, and subsequent recovery/rebuild can succeed.
4. Force the Nth `VulkanHostBuffer` allocation to fail. Check that all earlier owned per-draw UBOs and the newly allocated pool are released.
5. Force `vkAllocateDescriptorSets` failure. Check the `ClearPool` path's idempotency (the inner failure handling and the outer catch may both invoke it).
6. Change material draw count, transparency, texture IDs, skin palettes, and viewport sizes across frames. Verify the renderer never binds a descriptor set from an obsolete pool while GPU work is pending.
7. Confirm uniform ABI/descriptor slot layout against the actual compiled GLSL/SPIR-V pipeline; CPU descriptor ordering alone cannot prove shader compatibility.

Run native validation layers on a real Vulkan adapter, record the exact code commit, device/driver, failure injection point, VkResult and observed allocations/destructions.

## 7. Next improvement: a truly transactional descriptor generation

For a strong guarantee **even on allocation failure**, allocate a completely new descriptor pool, descriptor sets and UBO owners in a detached `DescriptorGeneration` RAII object. Validate and populate the entire candidate before publishing it into `VulkanMaterialDescriptors`. At a fence-safe boundary, atomically swap the candidate with the old generation; the old generation then destructs after the GPU has finished using it.

That design introduces a real generation ID (and possibly a deferred-retirement queue if multiple frames are in flight), avoiding premature destruction when an editor previews one material while simultaneously updating another.

The current source intentionally stops short of that larger refactor. It now guarantees **validation before mutation** and **immediate cleanup of partial native construction**, and it documents that preserving old descriptor sets across out-of-memory failures remains future work.
