# One texture's journey — from KairoAssets bytes to Vulkan shader sampling

**A deep C++23 and GPU-synchronization laboratory.** Start with the actual [KairoAssets texture artifact source](https://github.com/swayam8624/KairoAssets/blob/main/TextureArtifact.cppm), then read [`VulkanTexture.cppm`](../VulkanTexture.cppm), [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm), [`VulkanTriangle.cppm`](../VulkanTriangle.cppm), [`VulkanViewportTarget.cppm`](../VulkanViewportTarget.cppm) and [`VulkanShadowMap.cppm`](../VulkanShadowMap.cppm). Continue to [native frame submission and retirement](NATIVE_GPU_SYNCHRONIZATION_AND_LIFETIMES_LAB.md).

This chapter is about **actual data, ownership and native GPU ordering**, not merely a conceptual render-graph plan. The code was inspected and annotated on the active branch. No Vulkan validation-layer output, native GPU execution, texture-image diff or performance benchmark was produced as part of this documentation pass.

## 1. Four stages with different owners

```text
KairoAssets / TextureArtifactData
   Format, ColorSpace, Semantic, [Mip{Width,Height,Pixels}, ...]
          |
          | ValidateTextureArtifactData (CPU and authored contract)
          v
RendererRuntime::CreateTexture
  -> VulkanTriangle::CreateTexture
      -> std::unique_ptr<VulkanTexture2D>
          |
          | VkImage + device-local VkDeviceMemory + VkImageView + VkSampler
          | local VulkanHostBuffer with HOST_VISIBLE|HOST_COHERENT bytes
          | transient VkCommandPool and VkCommandBuffer
          | UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY
          | vkQueueSubmit + vkQueueWaitIdle
          v
VulkanTriangle::SetRenderScene
  -> m_DescriptorsDirty
  -> RebuildMaterialDescriptors (at retired frame boundary)
      -> VkDescriptorSet with borrowed ImageView/Sampler
      -> owned material and optional skin UniformBuffers
          |
          | VulkanTriangle::BindMaterial(drawIndex)
          v
VkCommandBuffer: draw indexed shaded mesh
          |
          v
Offscreen target: RGBA16F color + R32_UINT object ID + D32 depth
          |
          +-> editor samples shader-readable RGBA16F viewport color
          +-> optional one-pixel TRANSFER_SRC object ID readback
          +-> optional RGBA16F-to-buffer capture with explicit barriers
```

`TextureArtifactData` contains portable bytes, not a GPU handle. `TextureHandle` identifies a backend-owned upload, but does not convey the `VkDeviceMemory` object or authorize use after destruction. `VulkanTexture2D` owns the native sampled image and its sampler; the descriptor set **borrows** these handles. A render-graph `RenderResourceHandle` is a different, graph-local logical scheduling identifier.

## 2. The asset must describe what its pixel bytes *mean*

KairoAssets supports `R8`, `RG8`, `RGBA8` and `RGBA16Float` texture pixel formats. Each artifact also declares `Linear` or `SRGB` color space and semantic roles such as `Color`, `Normal`, `Data` and `HDR`.

The validation rules in [KairoAssets `ValidateTextureArtifactData`](https://github.com/swayam8624/KairoAssets/blob/main/TextureArtifact.cppm) matter before any Vulkan call:

- The texture has **1–16 mips**, and each mip is the exact downsampled width/height sequence (`w = max(1, w/2)`; similarly for `h`).
- The byte size of every mip equals `width * height * bytesPerPixel`; a purported 256×256 RGBA8 image must contain exactly 262,144 bytes at mip 0.
- Dimensions must be nonzero and no greater than the asset format's safety bound. Total decoded bytes have an independent payload cap.
- Normal, data and HDR texture semantics require **linear** color space; HDR requires `RGBA16Float`.

Why this is not cosmetic: an albedo value encoded with the sRGB transfer function is not numerically equivalent to a linear normal-map X component. `VulkanTexture2D::SelectFormat` maps the artifact's format and color space to `VK_FORMAT_*_SRGB`, `*_UNORM` or `VK_FORMAT_R16G16B16A16_SFLOAT`. A sampled sRGB format performs the appropriate nonlinear-to-linear interpretation; using it for normals or metalness can silently corrupt the shader's mathematics.

KairoAssets' CPU mip generator also treats data semantics appropriately: sRGB colors are averaged in linear space, while normal-map vectors are renormalized. **The GPU upload does not generate missing mips.** It consumes the validated precomputed bytes in the artifact's existing order.

## 3. Derive the staging offsets by hand

A simple RGBA8 4×4 texture with a complete down-to-one mip chain has three mip extents:

| Mip level | Extent | Bytes | Staging offset |
| --- | --- | ---: | ---: |
| 0 | 4×4 | 64 | 0 |
| 1 | 2×2 | 16 | 64 |
| 2 | 1×1 | 4 | 80 |
| **Total** | | **84** | |

`VulkanTexture2D::Upload` sums each `TextureMipLevel::Pixels.size()`, allocates a `VulkanHostBuffer` of that byte count with `VK_BUFFER_USAGE_TRANSFER_SRC_BIT`, then concatenates the mip byte vectors into `packed`. A single `staging.Write` copies them into the persistently mapped allocation.

The recorded `VkBufferImageCopy` array describes each mip's byte offset and subresource extent. Notice that `VkBufferImageCopy` is a **description of a future GPU copy**; it does not move bytes until the command buffer is submitted and executed.

If mip 1 erroneously claimed 3×2 pixels, `ValidateTextureArtifactData` would reject it before any VkImage was allocated. If it had 12 bytes rather than 16, it would also fail. These preconditions are preferable to discovering an invalid byte range only in a GPU validation callback.

## 4. A Vulkan image needs allocation, views, usage and a layout

`CreateImage` checks the physical device can sample the requested optimal-tiling format. It then creates a 2D `VkImage` with exactly the artifact's number of mips, `VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`, and initial layout `VK_IMAGE_LAYOUT_UNDEFINED`.

The image's storage comes from separately allocated device-local `VkDeviceMemory`: get image requirements, select a memory type whose `memoryTypeBits` permit `DEVICE_LOCAL`, allocate and bind. The native `VkImageView` spans every mip, and the sampler uses linear min/mag filtering plus linear mip interpolation, clamping maximum LOD to the last authored mip. U/V addressing follows the validated `TextureSampler`; no anisotropic sampling policy is promised.

All four objects—image, memory, view and sampler—belong to **one** `VulkanTexture2D` instance. Its constructor wraps native creation in a try/catch and calls `Destroy()` on partial failure, which matters because the enclosing C++ object's destructor is *not* invoked when its constructor throws.

## 5. The two real native image barriers

The temporary upload command pool/command buffer records a concrete GPU sequence:

```text
VkImageMemoryBarrier #1:
    oldLayout = UNDEFINED
    newLayout = TRANSFER_DST_OPTIMAL
    srcStage = TOP_OF_PIPE
    dstStage = TRANSFER
    srcAccess = 0
    dstAccess = TRANSFER_WRITE

vkCmdCopyBufferToImage(staging, image, TRANSFER_DST_OPTIMAL,
                       one VkBufferImageCopy for each mip)

VkImageMemoryBarrier #2:
    oldLayout = TRANSFER_DST_OPTIMAL
    newLayout = SHADER_READ_ONLY_OPTIMAL
    srcStage = TRANSFER
    dstStage = FRAGMENT_SHADER
    srcAccess = TRANSFER_WRITE
    dstAccess = SHADER_READ
```

The first transition permits writing the previously undefined image as a transfer destination. The copy populates each subresource. The second publishes the transfer writes for later fragment shader sampling.

These are **actual Vulkan commands** emitted by `vkCmdPipelineBarrier`. By contrast, the generic `RenderGraph` can describe a `ShaderRead` transition and invoke host hooks without allocating an image or emitting any Vulkan barrier. A correct backend must translate logical usage to concrete stages, access masks, layouts and queue-family rules.

After recording, the uploader performs `vkQueueSubmit` followed by `vkQueueWaitIdle`; it then destroys the temporary command pool and returns, allowing the staging buffer to be destroyed. This is deliberately synchronous and simple. For throughput-sensitive streaming, the future design would instead use a transfer queue, upload batch, per-upload completion synchronization, staging-buffer lifetime tracking and deferred texture publication.

**Error-path audit:** if the submission succeeded but `vkQueueWaitIdle` reports a failure, the current catch unconditionally destroys the command pool. Under a device-lost or other in-flight error, resource retirement semantics need native test and recovery design rather than assuming the GPU is finished. Do not infer normal-path failure from that source inspection alone.

## 6. A descriptor is a borrowed resource binding, not an owner

In [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm), a descriptor-set layout establishes binding types expected by shaders. The per-draw `VulkanMaterialDescriptors` layout currently has ten slots:

| Set binding | Resource | Shader use |
| --- | --- | --- |
| 0 | Camera/frame UBO | vertex and fragment |
| 1 | Directional shadow depth sampler | fragment |
| 2 | Base color texture | fragment |
| 3 | Normal texture | fragment |
| 4 | Metallic/roughness texture | fragment |
| 5 | Emissive texture | fragment |
| 6 | Occlusion texture | fragment |
| 7 | Material factors UBO | fragment |
| 8 | Environment texture | fragment |
| 9 | Optional skin-joint palette UBO | vertex |

Each material input `VulkanMaterialImages` holds **nonowning** image views/samplers and an optional pointer to the CPU `SkinPalette`. The descriptor rebuilding method copies skin matrices to an *owned* uniform buffer before GPU submission; it does not expose a pointer into a frame-local array to the shader. A uniform/image descriptor records which resources the GPU may access, but it does not prolong their lifetime just because a handle is stored inside a descriptor set.

For material channels with no authored texture, `VulkanTriangle::TextureOr` selects a 1×1 fallback (white, normal or black as appropriate). **Missing optional handle** and **nonzero invalid handle** are intentionally different: the latter is an error. `SetRenderScene` also validates referenced mesh handles, palette coverage and all material/environment texture handles.

`VulkanTriangle::RebuildMaterialDescriptors` runs when the current scene/texture bindings change, after the prior submitted command buffer has retired at the frame fence. It builds all five sampled image bindings per draw, includes the environment texture, and asks `VulkanMaterialDescriptors::Rebuild` to replace its sets/pool/uniform buffers. The per-draw set is then bound by `BindMaterial(drawIndex)` while recording native draw calls.

**Failure-path audit:** the rebuild routine destroys its previous pool before validating and allocating every new draw. A later invalid texture/material or allocation error can leave an incomplete set collection and no previous valid descriptors. This is **not strong exception safety**. A future transactional rebuild should build a complete new pool and buffers into local RAII holders and publish them only after all allocations and validation succeed.

## 7. Scene color, object IDs and depth are three different native images

The editor viewport's [`VulkanViewportTarget`](../VulkanViewportTarget.cppm) owns three distinct device-local attachments:

| Attachment | Format | Native usage | Final layout |
| --- | --- | --- | --- |
| HDR scene color | `R16G16B16A16_SFLOAT` | color attachment, sampled, transfer source | `SHADER_READ_ONLY_OPTIMAL` |
| Object ID | `R32_UINT` | color attachment, transfer source | `TRANSFER_SRC_OPTIMAL` |
| Depth | `D32_SFLOAT` | depth attachment | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` |

The render pass declares actual attachment initial/final layouts and two external subpass dependencies. It publishes color writes for later fragment shader reads and transfer reads. The object-ID texture is not a normal RGB color: it stores a stable per-draw integer for editor picking, and the picking path records `vkCmdCopyImageToBuffer` for a **single pixel**. The CPU reads it only at the following fence-retired frame.

For a full viewport capture, `VulkanTriangle::Record` temporarily transitions the HDR color image from shader-read-only to transfer-source layout, records a buffer copy of the full RGBA16F image, then transitions it back to shader-read-only for editor display. The consumer later converts the half-floats into 8-bit RGBA for display, which is *not* a lossless HDR export.

`VulkanViewportTexture` returns a **borrowed** sampler/image-view description that includes a generation counter. A non-noop resize destroys and recreates extent-dependent attachments and advances the generation; editor integrations must invalidate descriptors that still refer to the old image view. The renderer must idle GPU work first. The current Resize implementation tears down the old attachments before creating replacements, so failure midway through rebuilding is another case where prior contents are not preserved automatically.

## 8. The directional shadow map follows a different layout rule

[`VulkanDirectionalShadowMap`](../VulkanShadowMap.cppm) owns a D32 image, view, depth-only render pass, sampler and framebuffer independent of swapchain dimensions. Its format-support check requires **both** depth attachment and sampled-image support.

Its native render-pass layout path is:

```text
UNDEFINED
  -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL  [write depth from light camera]
  -> DEPTH_STENCIL_READ_ONLY_OPTIMAL  [sample as a shadow map in fragments]
```

Two external subpass dependencies publish fragment reads and depth writes across this transition. Unlike the regular image uploader, the shadow pass uses render-pass attachment transitions and dependencies rather than recording a new explicit `VkImageMemoryBarrier` around each draw.

The sampler uses nearest filtering and clamp-to-border, with an opaque-white border value. The descriptor declares `DEPTH_STENCIL_READ_ONLY_OPTIMAL`. The shadow pass must actually run (or otherwise initialize the depth image appropriately) before the material shader relies on its contents. A declared image layout is not evidence that valid depth data was written.

### Companion: a depth-only attachment is not a sampled shadow map

[`VulkanDepth.cppm`](../VulkanDepth.cppm) has a separate `VulkanDepthAttachment` RAII object for simple framebuffer depth. Its `D32_SFLOAT` image uses **only** `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, has one mip, and exposes a depth-aspect view. It does not have a texture sampler or sampled-image usage; trying to bind it as if it were `VulkanDirectionalShadowMap` would break the usage contract.

This small class has an important C++ exception-safety difference from `VulkanTexture2D`: its constructor calls `Create` without an enclosing catch-and-`Destroy`. If `vkCreateImage` succeeds but allocation/binding/image-view creation throws, the incomplete object's destructor is not invoked, so already-created Vulkan resources may leak. Its `Recreate` also destroys the old attachment before building a replacement; a failed rebuild cannot preserve the previous view. Correct these separately with scoped native handle guards and injected-allocation-failure tests.

## 9. Three different ownership boundaries to trace while debugging

**Authored ownership:** KairoAssets produces canonical texture bytes and a stable source-asset reference. It does not own the current renderer's VkImage.

**Renderer ownership:** `VulkanTriangle` stores a map from `TextureHandle` to `unique_ptr<VulkanTexture2D>`. `DestroyTexture` refuses deletion if a currently submitted draw/environment still references that handle. The caller's Vulkan renderer frame manager must also retire *older in-flight* GPU work before destroying or replacing the underlying native objects.

**GPU use:** descriptor sets borrow the image view, sampler and UBO allocations while recorded/submitted draws use them. RenderGraph lifetimes describe CPU-pass/resource relationships; VkQueue submission, render-pass transitions and completion fences establish the actual GPU access order.

These boundaries also apply to another backend (Metal), but not with identical API objects: [`detail/MetalBackend.mm`](../detail/MetalBackend.mm) uses `MTLTexture`, `replaceRegion` for uploaded mip bytes and command-buffer completion for safe readback/destruction. Comparing those paths is useful; pretending both use Vulkan barriers is not.

## 10. Tests that exist, and tests that remain missing

The checked-in [KairoRenderer unit tests](../tests/RendererTests.cpp) validate material channel contracts, render-scene references, render-graph hazards and backend policy. [`RenderGraphExecutionPlanTests.cpp`](../tests/RenderGraphExecutionPlanTests.cpp) verifies *logical* transition hook order and resource planning. The native [Vulkan skinning smoke test](../tests/VulkanSkinningSmoke.cpp) verifies rendered pixels change with a new joint palette when a present-capable GPU path is available. These fixtures are useful, but the checked-in tests are **not an exhaustive Vulkan texture-upload or descriptor-lifetime fault-injection suite**.

A useful next native validation suite would:

1. Upload known 4×4 RGBA8 pixels with explicit 4×4, 2×2 and 1×1 mips and compare captured sampled colors to a CPU reference, allowing sRGB/linear conversion and filtering tolerance.
2. Exercise each supported format and color-space combination, especially a linear normal texture versus sRGB albedo, with one-pixel and edge-clamp cases.
3. Supply malformed mip sizes, invalid semantic/color-space combinations, missing image-sampling support and incompatible physical-device memory types; confirm failure occurs at the right boundary without leaked VkHandles.
4. Rebuild descriptors after changing the scene and texture handles; verify previous command buffers are retired, stale handles are rejected and optional fallbacks are stable.
5. Repeatedly resize the viewport, validate the generation counter, and confirm old editor descriptors are rebuilt rather than used after image-view destruction.
6. Check the object-ID single-pixel copy and full HDR capture separately; compare GPU values to correctly formatted CPU expectations.
7. Inject allocation failures during texture construction, descriptor set rebuild, image-view construction, and rendering/submit paths; verify strong cleanup and usable error recovery.
8. Capture Vulkan validation-layer messages for all image layout/access transitions, including the shadow map and an enabled+disabled shadow toggle.

On a suitable host with dependencies, the build declares C++23 renderer tests and conditional native smoke executables. Source-accurate baseline commands are:

```bash
cmake -S . -B build -G Ninja \
  -DKAIRO_RENDERER_BUILD_TESTS=ON \
  -DKAIRO_RENDERER_ENABLE_VALIDATION=ON
cmake --build build --parallel 4
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
```

Keep host compiler version, driver, physical GPU, Vulkan loader, presentation surface and exact commit SHA with the result. **No test has been claimed to pass merely because its source was read or linked.**

## 11. Design extensions after understanding the current path

The current synchronous texture upload is a good teaching baseline. A scalable material streaming system would stage multiple uploads per command buffer, batch the two barriers (or use synchronization2), issue uploads asynchronously, and maintain completion-timeline/fence retirement before freeing staging memory or publishing descriptors. A descriptor cache would need invalidation keyed by texture generation, draw/material version, and device instance; an allocator would have to handle real native image granularity and transient alias compatibility.

Before implementing these changes, write down exactly **who owns the Vulkan objects, who borrows each handle, which native command accesses it, what signals completion, and which errors are recoverable**. That discipline is more valuable than adding a fast path without a lifetime proof.
