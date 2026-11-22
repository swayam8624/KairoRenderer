# From mip pixels to a Vulkan staging command: byte-accurate uploads and lifetime gates

This laboratory extends [the canonical texture to shader sampling chapter](TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md) with an **actual correctness refactor** in [`VulkanTexture.cppm`](../VulkanTexture.cppm). Read the CPU-only [`TextureMipUploadPlan.hpp`](../detail/TextureMipUploadPlan.hpp), its [fault-and-boundary regression](../tests/TextureMipUploadPlanTests.cpp), the [KairoAssets texture format](https://github.com/swayam8624/KairoAssets/blob/main/TextureArtifact.cppm), and [`VulkanHostBuffer`](../VulkanBuffer.cppm) alongside this lesson.

The production uploader now invokes the same plain-C++ mip staging planner as the regression test. This eliminates a duplicated byte-offset calculation and validates the total upload size before creating a temporary Vulkan buffer.

## 1. What exactly are we uploading?

A PNG or TGA is **not** what Vulkan samples. KairoAssets first parses source image bytes into a `TextureArtifactData` containing a validated format, semantic color-space interpretation and a vector of explicit mip levels. Each mip owns tightly packed texel bytes.

Think of an image as a stack of progressively smaller pictures. At distance the GPU can sample the smaller pictures, avoiding aliasing and unnecessary texture bandwidth. A 4×4 RGBA8 image with mips down to 1×1 contains:

| Mip | Dimensions | Texels | Bytes/texel | Bytes | Start byte |
| --- | --- | ---: | ---: | ---: | ---: |
| 0 | 4×4 | 16 | 4 | 64 | 0 |
| 1 | 2×2 | 4 | 4 | 16 | 64 |
| 2 | 1×1 | 1 | 4 | 4 | 80 |
| **Total** | | 21 | | **84** | |

The three mip images need not occupy three independently allocated staging buffers. We can pack their bytes into one contiguous 84-byte upload buffer and issue a `vkCmdCopyBufferToImage` call with **three** `VkBufferImageCopy` descriptors.

The offset for mip (i) is the sum of bytes in every earlier mip:

[
  o_i = \sum_{j=0}^{i-1} b_j, \qquad
  B = \sum_{i=0}^{n-1} b_i.
]

The Vulkan driver does not infer these offsets from the mip index. The caller has to provide them correctly, in units of **bytes**, via `bufferOffset`.

## 2. Separate the CPU plan from the Vulkan API

[The new shared C++23 header](../detail/TextureMipUploadPlan.hpp) constructs a `TextureMipUploadPlan` with two fields: `PackedBytes`, one vector holding the copied mip data, and `Regions`, one `{ByteOffset, Width, Height}` value per mip.

```cpp
// Illustrative pseudo-asset fixture. Production passes texture.Mips directly.
const auto upload = detail::MakeTextureMipUploadPlan(texture.Mips);

// One contiguous host-visible upload buffer, not one buffer per mip.
VulkanHostBuffer staging(
    device,
    static_cast<VkDeviceSize>(upload.PackedBytes.size()),
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

staging.Write(upload.PackedBytes.data(),
              static_cast<VkDeviceSize>(upload.PackedBytes.size()));

// Later, inside command-buffer recording:
for (std::uint32_t level = 0; level < upload.Regions.size(); ++level)
{
    const auto& region = upload.Regions[level];
    VkBufferImageCopy copy{};
    copy.bufferOffset = static_cast<VkDeviceSize>(region.ByteOffset);
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u};
    copy.imageExtent = {region.Width, region.Height, 1u};
    copies.push_back(copy);
}
```

This separation is useful even when learning Vulkan for the first time: **CPU memory layout** and **GPU image layout** are different concepts. The planner owns byte offsets in ordinary host memory. The subsequent `VkImageMemoryBarrier` commands establish a native device image layout (`TRANSFER_DST_OPTIMAL`, then `SHADER_READ_ONLY_OPTIMAL`). Neither mechanism substitutes for the other.

## 3. Validate the arithmetic before allocating

Even well-typed inputs can fail at arithmetic boundaries. An unguarded loop such as `totalBytes += mip.Pixels.size()` can mathematically overflow a fixed-width unsigned integer, producing a staging allocation smaller than the required data. A wrapped offset could make multiple mip copies read the wrong part of the buffer.

The new planner checks:

- At least one mip exists and the number of levels fits a `uint32_t` Vulkan subresource count.
- Every declared mip has nonzero dimensions and nonempty bytes.
- The total fits the chosen `uint64_t` upload budget and native `VkDeviceSize` offset width, **before addition**.
- The total fits a `std::vector<std::byte>` on the actual host, **before** any reserve/insert.
- A single computed `ByteOffset` is used both to describe and to test the corresponding packed byte location.

`ValidateTextureArtifactData` **still** owns the stricter format-specific checks: canonical halved dimensions, bytes per pixel, 16-mip maximum, semantic restrictions, and its payload budget. The staging planner intentionally does not duplicate all KairoAssets color-space and format knowledge.

## 4. A real source-backed regression fixture

[The new CPU regression](../tests/TextureMipUploadPlanTests.cpp) supplies three mip vectors with distinct values:

```text
4x4 RGBA8: 64 bytes all 0x11
2x2 RGBA8: 16 bytes all 0x22
1x1 RGBA8:  4 bytes all 0x33

expected total length: 84
expected buffer offsets: 0, 64, 80
expected last packed 4 bytes: 0x33 0x33 0x33 0x33
```

It checks exact copied pixel ranges, every extent and offset, the **exactly-84-byte** allowed budget, rejection at **83 bytes**, empty chains, missing pixel bytes, zero-width mip and an artificially small upload budget. Rejections happen without making a native VkImage or allocating giant test textures.

The exact compiled regression target is `KairoRendererTextureMipUploadPlanTests`. It is registered in [CMake](../CMakeLists.txt) and the [GCC/Clang CPU CI workflow](../.github/workflows/cpu-render-resource-guards.yml). The [KairoGameEngine standalone six-test runner](https://github.com/swayam8624/KairoGameEngine/blob/main/scripts/run_renderer_resource_lifetime_regressions.sh) includes this test when the sibling KairoRenderer checkout is available.

**Evidence boundary:** the portable test proves the actual CPU planner's offsets and copies. It cannot prove the Vulkan driver accepted the image format, the copy actually executed, the barriers synchronized fragment reads, or the shader sampled expected colors. Those remain native GPU tests.

## 5. Why the two Vulkan image barriers matter

For an image created with `VK_IMAGE_LAYOUT_UNDEFINED` and usage `VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`:

```text
vkCmdPipelineBarrier #1
    old = UNDEFINED
    new = TRANSFER_DST_OPTIMAL
    dst access = TRANSFER_WRITE

vkCmdCopyBufferToImage
    buffer = staging (84 bytes in example)
    image = VkImage (3 native mip subresources)
    region offsets = [0, 64, 80]

vkCmdPipelineBarrier #2
    old = TRANSFER_DST_OPTIMAL
    new = SHADER_READ_ONLY_OPTIMAL
    src access = TRANSFER_WRITE
    dst access = SHADER_READ
```

The sample implementation records all these into one transient command buffer and submits it on the graphics queue. The first barrier makes the image writable, the copies populate mip subresources, and the second makes those writes visible to later shader reads.

The material descriptor can then bind a sampled image view that covers the complete mip pyramid. sRGB decoding depends on the selected Vulkan `VkFormat`, not on changing these byte offsets.

## 6. Upload completion is a separate lifetime proof

The current uploader intentionally waits synchronously:

```text
Create VulkanHostBuffer staging
Create transient VkCommandPool and VkCommandBuffer
Record transitions and copy
vkQueueSubmit(...)
vkQueueWaitIdle(graphicsQueue)
Destroy transient command pool
Destroy staging buffer (on scope exit)
Return uploaded VkImage/Memory/View/Sampler
```

On **normal success**, the queue idle wait establishes that the transient command buffer and staging memory are no longer needed by its queue submission. This is slow for large streaming workloads, but relatively easy to reason about.

**Later source-level correction:** the [upload queue retirement and device-loss laboratory](VULKAN_UPLOAD_QUEUE_RETIREMENT_AND_DEVICE_LOSS_LAB.md) documents the now-implemented `SubmitAndRetireVulkanUpload` policy. Ordinary success, OOM-rejected submission, confirmed device idle and `VK_ERROR_DEVICE_LOST` have separate cleanup rules. When neither device completion nor device loss is established, the command pool, staging buffer/memory, image/memory, view and sampler transfer into `VulkanDevice`'s deferred journal instead of being immediately destroyed by constructor unwinding. Rendering is disabled until the caller constructs a new device. This is a concrete lifetime fix; it does **not** certify native Vulkan failure injection, real lost-device reconstruction, or guaranteed recovery after ambiguous errors.

## 7. The path toward a streaming uploader

For thousands of textures, the current per-texture `vkQueueWaitIdle` is a serialization bottleneck. An advanced uploader should batch multiple `VkBufferImageCopy` operations into shared transfer command buffers, use a completion fence/timeline value to track the lifetime of *every* staging allocation, maintain resource-generation publication so shaders never sample unfinished images, and use synchronization2 barriers/queue-family ownership transfers when applicable.

Such a system is not “replace `vkQueueWaitIdle` with nothing.” Removing the wait without establishing another completion mechanism would make temporary command-pool and staging-buffer destruction incorrect even on otherwise healthy GPUs.

Start with the supplied 84-byte CPU fixture, reproduce actual shader output on a native host, then move to asynchronous execution with measurable correctness and performance baselines. The new staging planner can be kept unchanged because its byte-accurate CPU contract is independent of the submission policy.
