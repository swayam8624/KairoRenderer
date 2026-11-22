# Vulkan texture upload retirement: the difference between submission, completion and device loss

This laboratory continues [the texture mip staging and native lifetime chapter](VULKAN_TEXTURE_MIP_UPLOAD_PLAN_AND_FAILURE_LAB.md). It accompanies real ownership changes across [`VulkanTexture.cppm`](../VulkanTexture.cppm), [`VulkanDevice.cppm`](../VulkanDevice.cppm), [`VulkanBuffer.cppm`](../VulkanBuffer.cppm) and [`VulkanRenderer.cppm`](../VulkanRenderer.cppm), not just new comments.

Read those files, then [`detail/VulkanUploadRetirementPolicy.hpp`](../detail/VulkanUploadRetirementPolicy.hpp) and [its exact CPU regression](../tests/VulkanUploadRetirementPolicyTests.cpp). The latter has no Vulkan dependency, and the production texture uploader invokes the same policy. The regression is registered in CMake and the GCC/Clang CPU workflow. A full native driver fault-injection run is **still necessary**.

## 1. The original lifetime error

The previous upload performed the equivalent of:

```cpp
VkCommandPool pool = CreatePool();
VulkanHostBuffer staging = CopyPixelsIntoHostVisibleMemory();
RecordMipBarriersAndCopies(pool, staging, destinationImage);

try
{
    if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
        vkQueueWaitIdle(queue) != VK_SUCCESS)
        throw std::runtime_error("upload failed");
}
catch (...)
{
    vkDestroyCommandPool(device, pool, nullptr);
    throw;                  // staging and destination texture also destruct
}
```

A successful `vkQueueSubmit` **does not mean the GPU has completed** the copy. If `vkQueueWaitIdle` then returns an unexpected error, throwing from the texture constructor unwinds the `VulkanHostBuffer` and destroys the image and command pool. Unless queue completion or Vulkan device loss is established, this destruction might violate native lifetime requirements.

The correction is not to ignore the wait error; it is to classify *which native fact* the returned result establishes.

## 2. What the Vulkan specification actually guarantees

The current Khronos specification describes two important facts:

- `vkQueueSubmit` returning `VK_ERROR_OUT_OF_HOST_MEMORY` or `VK_ERROR_OUT_OF_DEVICE_MEMORY` must not alter the referenced command buffers/resources/semaphores. Other failures that cannot preserve that condition must be reported as `VK_ERROR_DEVICE_LOST`.
- For command-buffer pending state and resources considered *in use*, Vulkan says `VK_ERROR_DEVICE_LOST` is treated like `VK_SUCCESS`. A lost device's native child handles are still valid and must be explicitly destroyed before `vkDestroyDevice`. This rule concerns **cleanup legality**, not texture-content correctness or permission to keep drawing.

Official references: [vkQueueSubmit](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html), [vkQueueWaitIdle](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueWaitIdle.html), [Lost Device and Device Destruction](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device).

This distinction is why merely branching on `result != VK_SUCCESS` and destroying everything in a catch is too coarse.

## 3. A CPU-only retirement policy used by native code

The new [`SubmitAndRetireVulkanUpload`](../detail/VulkanUploadRetirementPolicy.hpp) accepts injectable callbacks for queue submission, queue idle, and last-resort device idle. It returns a `VulkanUploadRetirement` status:

| Status | Meaning | Native ownership action |
| --- | --- | --- |
| `Completed` | Submit succeeded; queue idle succeeded | Normal pool/staging/image lifetime |
| `Rejected` | Submit failed with OOM and did not alter referenced GPU work | Destroy unsubmitted temporary objects; report error |
| `RetiredAfterError` | Unexpected submit/wait failure, but device-wide idle later succeeded | Cleanup is now safe; preserve original failure |
| `DeviceLost` | Submit/wait/fallback reported device loss | Cleanup is legal under lost-device lifetime rule; mark renderer unusable |
| `Quarantine` | Neither successful completion nor lost device was established | **Do not destroy** pool, staging buffer, or destination texture; transfer their handles into device-level deferred ownership |

The default happy path retains the original single synchronous queue-idle strategy, so the change does not introduce partially uploaded textures into the material system.

For unknown submit errors, the policy also attempts device idle. A failed VkResult does not grant permission to guess whether commands were ever submitted.

## 4. The device owns the ambiguous case

A `VulkanTexture2D` object cannot safely keep pending resources inside its own partially constructed body: when its constructor throws, its destructor does not run and its normally constructed C++ members are unwound.

The new code therefore separates two actions:

```text
BEFORE vkQueueSubmit
    ReserveDeferredTextureUploadSlot()      // may allocate; nothing queued
    create mapped staging, VkCommandPool, VkImage, VkView, VkSampler

AFTER command buffer recording
    SubmitAndRetireVulkanUpload(...)

    if Completed:
        normal texture construction succeeds
        ordinary staging/command-pool cleanup

    if Rejected, RetiredAfterError, or DeviceLost:
        ordinary native cleanup
        mark logical device unusable for DeviceLost
        throw a diagnostic for the failed upload

    if Quarantine:
        staging.DetachForDeferredRetirement()
        VulkanDevice.DeferUncertainTextureUpload(all native handles)
        zero every handle on VulkanTexture2D and local command pool
        mark logical device unusable
        throw while C++ owners hold NO unsafe native resource
```

The deferred record contains the command pool, mapped staging buffer and memory, destination texture image and memory, image view, and sampler. Each handle is transferred **once**: after `DetachForDeferredRetirement`, `VulkanHostBuffer::~VulkanHostBuffer` has nothing left to free; after zeroing the texture handles, the incomplete texture constructor's error handler is safe.

The device's deferred vector has reserved capacity **before** submission, so recording a failure cannot itself throw `std::bad_alloc` after GPU work began. The code assumes the existing externally serialized graphics queue; concurrent callers must use the same queue/ownership serialization.

When `VulkanDevice` is eventually destructed, it calls `vkDeviceWaitIdle` before releasing those deferred objects. If idle succeeds, the resources are retired normally. If the driver reports `VK_ERROR_DEVICE_LOST`, the Vulkan lifetime rule also allows cleanup. If the device cannot establish either result, the destructor **fails closed** with a diagnostic and `std::terminate` rather than destroying possibly in-use resources or a parent device with still-live children. This is an explicit last-resort failure policy, **not** graceful device recovery.

## 5. Fail-fast rendering and normal shutdown

A GPU upload that reports `VK_ERROR_DEVICE_LOST` or enters the ambiguous quarantine state marks the owning `VulkanDevice` unusable. Its public `CheckUsable` gate now blocks further texture/mesh allocations; `VulkanRendererRuntime::DrawFrame` refuses another frame. This stops the application from trying to render using a device whose state cannot support another safe submission.

The renderer destructor now attempts `vkDeviceWaitIdle` in its **destructor body**, before its C++ member destructors release render pipelines, swapchain views, frame synchronization, and buffers. This fixes the normal ownership ordering when the last frame might still be pending. The same lost-device result is recognized as a cleanup boundary. An unprovable idle failure at this teardown gate is fatal for the same reason as above.

Explicit `DestroyMesh`/`DestroyTexture` also treat a lost-device result as permitting child cleanup, while preventing future rendering on the lost device. They still enforce the *authored render scene* reference checks: a texture referenced by the submitted scene cannot be destroyed merely because the device is idle.

**This is not device recreation.** There is no attempt to rebuild the Vulkan logical device, reload assets, rebind editor descriptors or resume frames after a device loss.

## 6. What the regression can prove

[`VulkanUploadRetirementPolicyTests.cpp`](../tests/VulkanUploadRetirementPolicyTests.cpp) fault-injects results that mimic Vulkan VkResult values and verifies:

1. Successful submission invokes one queue-idle callback and needs no device fallback.
2. Host/device OOM submission errors bypass queue wait because no submission was published.
3. `VK_ERROR_DEVICE_LOST` from submission or queue wait returns a safe-cleanup, unusable-device disposition.
4. An unexpected queue wait error triggers one device-idle fallback.
5. A successful fallback retires the upload but still reports the original error.
6. Device loss during fallback permits retirement.
7. Neither idle completing causes `Quarantine`, not ordinary cleanup.
8. An unknown submission failure requires an idle check rather than treating all submit errors like OOM.

The test uses the **same decision helper invoked by `VulkanTexture2D::Upload`**. It does not construct or destroy real `VkImage` objects, so it cannot prove the native deferred journal's image/memory order, device-loss driver behavior, or WSI interactions.

## 7. Native verification checklist

The following are **outstanding tests**, not results established by this source pass:

- Run a normal three-mip upload, sampled PBR material and rendered image with Vulkan validation layers enabled.
- Force `vkQueueSubmit` to return `VK_ERROR_OUT_OF_HOST_MEMORY`: no queue wait and no retained handle.
- Force `vkQueueWaitIdle` to return an error while `vkDeviceWaitIdle` succeeds: check pool/staging/texture destruction is delayed until that success.
- Force both waits to return recoverable errors: assert the deferred device journal owns all eight handles and the original C++ objects own none; no additional frame or upload is accepted.
- Force `VK_ERROR_DEVICE_LOST` at submit, wait and shutdown: verify lost-device cleanup order and native validation traces.
- Exercise teardown with the last submitted *render* frame still running, not just the upload queue. Confirm renderer's destructor-body idle precedes member destruction.
- Run sanitizers plus a fake Vulkan dispatch layer with per-handle creation/destroy counters to prove no double free or handle loss.
- Test genuine physical-device loss separately; a successfully created replacement logical device is not guaranteed.

The CPU-only policy test is registered as `KairoRendererVulkanUploadRetirementPolicyTests` in [CMake](../CMakeLists.txt), included in [the renderer GCC/Clang CI](../.github/workflows/cpu-render-resource-guards.yml), and callable via [KairoGameEngine's seven-test standalone C++23 runner](https://github.com/swayam8624/KairoGameEngine/blob/main/scripts/run_renderer_resource_lifetime_regressions.sh).

## 8. The deeper C++ lesson

Native resource safety requires more than RAII alone. RAII only knows the handles it owns; it does not automatically know whether the **GPU is still using them**. The safe ownership sequence is:

```text
Allocate → Record → Submit → Establish completion OR device loss
                                  ↓
             Release immediately OR defer until a valid retirement gate
```

This is the same principle used by explicit rendering engines to build deferred deletion queues, transient upload arenas, per-frame descriptor generations and streaming textures. A queue submission is not a completed computation, and a device-loss error is not a successful frame—even when the Vulkan specification allows native cleanup afterward.
