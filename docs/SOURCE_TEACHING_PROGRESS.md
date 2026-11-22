# KairoRenderer source-teaching coverage — texture and GPU-resource continuation

**Dated:** November 22, 2026. This ledger records comments and documentation present on the active `main` branch; it is **not** a claim of native GPU test success, complete source annotation or an automatically merged historic PR.

**Descriptor safety follow-on:** [VulkanMaterialDescriptors::Rebuild now validates every input before pool retirement and clears partially built native allocations on failure](VULKAN_DESCRIPTOR_REBUILD_ERROR_SAFETY_LAB.md). The old generation is still lost if a *native allocation* fails after validation; full strong transaction is future work. **CI:** [CPU-only GCC/Clang workflow](../.github/workflows/cpu-render-resource-guards.yml) is committed, not a claim of completed CI/native GPU tests.

**Vulkan frame recovery update:** [acquire/record/submit/present failure state journal and native recovery logic](VULKAN_FRAME_FAILURE_RECOVERY_LAB.md) is now on `main`. The fence reset occurs only after recording succeeds; unconsumed acquired semaphores are drained before swapchain generation retirement. A CPU-only test models the state order, but live WSI/driver validation and device-loss reconstruction remain unverified.

**Later Vulkan repair update:** [partial constructor allocation rollback for five native wrappers](VULKAN_PARTIAL_CONSTRUCTION_ROLLBACK_LAB.md) is now committed. The older audit list below records the original findings; native fault-injection/validation-layer evidence and fence-reset recovery remain outstanding.

## Later functional GPU resource cleanup repairs

[Cross-repository rollback/teardown guide](https://github.com/swayam8624/KairoGameEngine/blob/main/docs/GPU_RESOURCE_ROLLBACK_AND_RENDERER_TEARDOWN_LAB.md) records subsequent functional changes: native Metal and Direct3D12 CreateMesh/CreateTexture use a tested publish-or-rollback helper; OpenGL cleans GL names on post-upload map/handle allocation failure; KairoPlayer now tracks created handles before binding and unbinds the submitted scene before release. [CPU-only native-publication regression test](../tests/NativeResourcePublicationTests.cpp) is registered in [CMake](../CMakeLists.txt). This source-level status is distinct from native GPU-driver testing.

## Completed in this continuation

| Source file | Added function/structure-level teaching sections | Concept and cross-file dependence |
| --- | ---: | --- |
| [`VulkanTexture.cppm`](../VulkanTexture.cppm) | 14 | KairoAssets TextureArtifactData → host staging buffer → device-local VkImage → full mip-chain image barriers → sampler/view |
| [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm) | 14 | Camera/shadow/PBR/environment/skin descriptor binding layout, borrowed views, owned uniform allocations and fence-safe descriptor rebuild |
| [`VulkanTriangle.cppm`](../VulkanTriangle.cppm) | 12 | Renderer texture handles, fallback policy, scene validation, descriptor dirtiness, offscreen render, pick and HDR capture copies |
| [`VulkanViewportTarget.cppm`](../VulkanViewportTarget.cppm) | 15 | RGBA16F scene color, R32_UINT object IDs, D32 depth, native render-pass dependencies, resize generation and borrowed editor image view |
| [`VulkanShadowMap.cppm`](../VulkanShadowMap.cppm) | 9 | Persistent sampled directional D32 map, depth-to-fragment-read layout, independent framebuffer lifecycle |
| [`VulkanDepth.cppm`](../VulkanDepth.cppm) | 7 | Unsampled depth-only attachment, creation/recreation error path, image/view/memory ownership |
| **Total** | **71** | **New implementation teaching sections** |

**New deep laboratory:** [From canonical mip bytes to Vulkan shader sampling, descriptor sets and image layouts](TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md). The [README](../README.md), [full source reading order](SOURCE_READING_ORDER.md) and [mesh-to-presented-frame walkthrough](FRAME_LIFECYCLE_WALKTHROUGH.md) link this course.

**Cross-repository source teaching:** [KairoAssets `TextureArtifact.cppm`](https://github.com/swayam8624/KairoAssets/blob/main/TextureArtifact.cppm) received 17 sections on format/semantic validation, binary16, color-space-correct mip generation and strict binary formats. Its [texture artifact tests](https://github.com/swayam8624/KairoAssets/blob/main/tests/TextureArtifactTests.cpp) received 5 fixture/assertion explanations. KairoAssets README/source map link the same GPU boundary lab.

## Required mental model

```text
AssetID                     [stable authored identity, KairoAssets]
  -> TextureArtifactData   [CPU-owned validated pixels/mips]
  -> TextureHandle         [opaque backend resource identity]
  -> VulkanTexture2D       [owns native VkImage/memory/view/sampler]
  -> VulkanMaterialImages  [BORROWS image view and sampler]
  -> VkDescriptorSet       [borrows GPU resources, owns no actual VkImage]
  -> VulkanTriangle.Record [native pass and VkCommandBuffer]
  -> VulkanRenderer        [GPU execution synchronization and readback]
```

The graph-only resource handle is NOT one of these GPU objects. `RenderGraph::Compile` may calculate a shader-read transition, but only native backend commands can establish Vulkan image layouts/access dependencies. All live GPU consumers must retire before any referenced VkView, sampler, buffer, descriptor pool or image is destroyed.

## Specific correctness and verification risks

These are **source-audit candidates**. No current-host failure has been demonstrated.

1. **Native partial-construction errors.** `VulkanDepthAttachment` can allocate image and memory and then throw without a constructor-level catch/cleanup guard. Some other Vulkan wrappers have independent failure-path risks documented in [native GPU synchronization laboratory](NATIVE_GPU_SYNCHRONIZATION_AND_LIFETIMES_LAB.md). Add scoped handle guards and fault-injection tests.
2. **Descriptor rebuild transactionality.** `VulkanMaterialDescriptors::Rebuild` clears old pool/UBOs before completing all new allocations and validation. It cannot guarantee preservation of prior descriptor sets if a new material fails midway. Rebuild on the fully retired frame and consider two-phase publish.
3. **Upload queue error path.** `VulkanTexture2D::Upload` waits for `vkQueueWaitIdle`, but the failure catch destroys its transient command pool even if submission already succeeded and completion could not be proven. Device-loss handling needs an explicit lifetime policy.
4. **Viewport resize recovery.** `VulkanViewportTarget::Resize` releases existing attachments before allocating replacements; editor descriptors must invalidate old generation views and handle an allocation failure.
5. **Real GPU layout evidence.** Check the two `VkImageMemoryBarrier` transitions in Vulkan texture upload; viewport render-pass attachment final layouts; offscreen HDR capture transfer barriers; directional shadow depth-to-sampled dependencies. Validate with Vulkan validation layer output.
6. **Texture parity.** Compare known mip-level pixel samples across canonical CPU artifact, Vulkan sampled-image shader and Metal output; account for sRGB-to-linear conversion, texture filtering and float precision rather than assuming every byte becomes an identical output value.
7. **Resource-leak / in-flight destruction.** Force errors after `vkCreateImage`, `vkAllocateMemory`, image-view/sampler allocation, descriptor pool creation, and command submission. Confirm no in-flight handles are freed early or left leaked.
8. **Coverage outside this pass.** The remaining `VulkanTriangle.cppm` shader pipelines, OpenGL and D3D12 texture paths, editor descriptor invalidation, asynchronous streaming/upload batching, audio/UI integration, and full native backend build verification still need dedicated passes.

## Test evidence boundary

Existing `KairoRendererTests` and render-graph tests exercise shader-independent contracts and host graph plans. The platform-gated `KairoRendererVulkanSkinningSmoke` and `KairoRendererMetalSkinningSmoke` are **native tests** when they actually run on supported hosts. This documentation pass did not run a native GPU build, validation layer, GPU profiler, or sanitizer. Do not infer operational success from a source link or compiled feature flag.

The real baseline on a configured native host is documented in the [texture upload lab](TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md). Record exact commit SHA, OS, GPU/driver, compiler, validation output, tested fixture and error tolerances whenever running these checks.
