# Vulkan constructor failure recovery: C++23 object lifetime and native handle rollback

This advanced follow-up to the [Vulkan/Metal native GPU lifetime laboratory](NATIVE_GPU_SYNCHRONIZATION_AND_LIFETIMES_LAB.md) documents concrete **functional cleanup fixes** across five Vulkan wrappers. Source review alone is not a validation-layer result; run the requested native tests on a configured Vulkan device.

The core C++ rule is simple but far-reaching: **if a constructor throws, its class destructor does not execute**, even though destructors of successfully constructed data members still do. A `VkBuffer` or `VkCommandPool` stored as a numeric raw handle is not an RAII data member. Without an explicit catch/rollback it leaks if an earlier `vkCreate*` succeeds and a later operation fails.

## 1. C++ subobject destruction versus native Vulkan ownership

Consider:

```cpp
class NativeThing {
    VkDevice device;
    VkImage image = VK_NULL_HANDLE;
public:
    explicit NativeThing(VkDevice d) : device(d) {
        if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS)
            throw std::runtime_error("create image");
        if (LaterAllocationCanFail() != VK_SUCCESS)
            throw std::runtime_error("allocate memory");
    }
    ~NativeThing() { vkDestroyImage(device, image, nullptr); }
};
```

If the second step throws, `~NativeThing()` does **not** run, so the Vulkan image remains live. `VkImage` is a plain member handle; its implicit member destruction does nothing.

The basic repaired pattern is:

```cpp
explicit NativeThing(VkDevice device) {
    try { CreateNativeObjects(); }
    catch (...) {
        DestroyNativeObjects(); // must tolerate partially initialized handles
        throw;
    }
}
~NativeThing() { DestroyNativeObjects(); }
```

A robust `DestroyNativeObjects()` checks each handle for `VK_NULL_HANDLE` and resets it after destruction, allowing it to be called during partial construction and ordinary teardown.

## 2. Actual source fixes

| Source | Before | After |
| --- | --- | --- |
| [`VulkanCommand.cppm`](../VulkanCommand.cppm) | `vkAllocateCommandBuffers` failure threw after creating `VkCommandPool` | Explicitly destroys pool and clears handle before rethrow |
| [`VulkanBuffer.cppm`](../VulkanBuffer.cppm) | `vkAllocateMemory` / `vkBindBufferMemory` / `vkMapMemory` failures could leak created buffer or memory | Constructor catches failures; shared `Destroy()` unmaps, destroys VkBuffer, frees VkDeviceMemory |
| [`VulkanSync.cppm`](../VulkanSync.cppm) | Failure creating initial acquire semaphore or fence was outside inner per-image catch | Constructor and `Recreate` now catch `Create` failures and call idempotent `Destroy` |
| [`VulkanSwapchain.cppm`](../VulkanSwapchain.cppm) | Failed image enumeration/view creation could leave swapchain or earlier views live | Constructor and `Recreate` catch and destroy; image-view vector capacity is reserved before allocating any native view |
| [`VulkanDepth.cppm`](../VulkanDepth.cppm) | Constructor or resize failure could leave partially allocated D32 image, memory or view | Constructor and `Recreate` catch and invoke partial-safe `Destroy` |

The VulkanSwapchain pre-reservation is a separate C++ pitfall: even after `vkCreateImageView` succeeds, `std::vector<VkImageView>::push_back` can allocate and throw before the handle is recorded. Reserving `m_Images.size()` elements **before** creating views means a successful VkImageView enters the journal without a vector allocation. If a later native view creation fails, `Destroy()` sees every earlier view.

## 3. Buffer lifetime has an ordering constraint

The `VulkanHostBuffer` cleanup routine now follows:

```text
if mapped:
    vkUnmapMemory(device, memory)
    mapped = nullptr
if buffer exists:
    vkDestroyBuffer(device, buffer)
    buffer = VK_NULL_HANDLE
if memory exists:
    vkFreeMemory(device, memory)
    memory = VK_NULL_HANDLE
size = 0
```

The prior destructor freed memory before destroying its bound buffer. The corrected order destroys the buffer first, then frees its allocation. That matters because a live bound Vulkan object must not outlast its backing memory.

**This does not by itself handle GPU in-flight access.** The caller must still finish any command queues that reference the buffer before executing the destruction routine. The constructor catch is safe because that partially built buffer was never returned to a renderer and never legitimately submitted for GPU use.

## 4. Why synchronization cleanup requires special care

[`VulkanFrameSync`](../VulkanSync.cppm) owns one acquire semaphore, a signaled first-frame fence and one render-complete semaphore per swapchain image. `Create` can fail at several points: invalid image count, acquire semaphore, fence, vector allocation or a later per-image semaphore.

A partial Vulkan semaphore/fence array should be destroyed in any of those failure cases; the same `Destroy` handles null slots without calling invalid Vulkan destruction functions. For `Recreate`, the existing contract still requires the device to have been idled externally before old sync objects are destroyed.

**Important distinction:** this pass fixes *sync object creation failure cleanup*. It does **not** repair the separate VulkanRenderer frame path in which the fence is reset after acquiring an image and a later recording/submission failure could leave that fence unsignaled indefinitely. That needs a frame-state recovery design and real fault-injection testing.

## 5. Depth and swapchain rebuild are not strong transactions

In `VulkanDepthAttachment::Recreate` and `VulkanSwapchain::Recreate`, the current implementation still destroys the previous native objects before creating replacement objects. The new catch ensures **the failed replacement's partial allocations are cleaned**, but the old framebuffer/image generation is already gone.

Therefore the operation offers cleanup safety, not the strong guarantee “failure preserves the old complete rendering state.” A future transactional approach could build a separate replacement generation, validate it, then swap ownership at a GPU-safe retirement boundary. For a swapchain this also requires `oldSwapchain` negotiation, present queue ownership, framebuffers, descriptors and potentially multiple frames in flight.

## 6. Failure injection matrix for the next native pass

| Injected failure point | Resource that must remain absent/clean | Evidence needed |
| --- | --- | --- |
| `vkAllocateCommandBuffers` | newly created VkCommandPool and buffer | Vulkan API spy/validation handles; test repeated constructor attempts |
| VulkanHostBuffer memory-type lookup, allocate, bind, map | VkBuffer, VkDeviceMemory and mapped range | verify destruction order and no leaked native allocations |
| Initial or Nth frame semaphore/fence | all prior semaphores, in-flight fence | per-handle destruction counts and idempotent cleanup |
| Swapchain image count/array allocation | VkSwapchainKHR, any image views | verify no view leaked even when vector capacity fails |
| Nth `vkCreateImageView` for swapchain | previous image views and swapchain | validate per-view ownership count |
| Depth image allocation/bind/view | depth image, allocation and view | failure retries under live device; no stale view retained |

Use a test build with instrumented Vulkan dispatch pointers or a dedicated fake Vulkan allocator; a mere thrown `std::bad_alloc` at the wrong location will not exercise a device API returning `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Record exactly which call failed and which handles the test observed.

On a genuine Vulkan host, enable validation layers, exercise repeated create/recreate/destroy cycles, resize while minimized/restored, and verify resource cleanup without using handles after their parent `VkDevice` dies.

## 7. Related cross-backend fixes and CPU tests

The [Player/renderer rollback guide](https://github.com/swayam8624/KairoGameEngine/blob/main/docs/GPU_RESOURCE_ROLLBACK_AND_RENDERER_TEARDOWN_LAB.md) explains the separate **after-successful-native-upload** failure: Metal/D3D12 and OpenGL native resource creation could succeed before a C++ handle map insertion fails. That is handled by a publish-or-rollback helper and a CPU fault-injection regression test.

These are distinct stages. Constructor rollback addresses failure before a wrapper is fully created; publication rollback addresses a fully created native resource not yet visible through a facade; frame retirement addresses GPU work already submitted with a resource.

The standalone [KairoGameEngine regression runner](https://github.com/swayam8624/KairoGameEngine/blob/main/scripts/run_renderer_resource_lifetime_regressions.sh) compiles the CPU-only Player and renderer tests using a C++23 compiler without a GPU window/device. Those CPU tests do **not** exercise any Vulkan resource create failures listed here.

## 8. Verification status and remaining work

The five Vulkan wrappers were edited on KairoRenderer `main` and their new catch/destroy paths inspected. No native Vulkan SDK or GPU was available in the current execution environment, so these changes have **not** been claimed to compile/run against a Vulkan driver in this pass.

The highest-priority remaining issues are the Vulkan fence-reset-before-submit recovery, device-loss and queue-error teardown policy, upload command-pool lifetime under `vkQueueWaitIdle` errors, and native validation-layer/fault-injection evidence. The normal-success and error-cleanup paths must both be tested, because an “extra cleanup” is not automatically safe if GPU commands may still reference a resource.

**Study takeaway:** the only reliable ownership story is a sequence of explicit state transitions—acquire, record in a nonthrowing owner journal, publish, submit, retire, unpublish, destroy. C++ destructors cannot discover a raw native handle that was created and then forgotten during an exception.
