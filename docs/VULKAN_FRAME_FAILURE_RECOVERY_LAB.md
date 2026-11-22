# Vulkan frame failure recovery — acquired images, binary semaphores, fences, and swapchain retirement

This is the continuation of [native GPU synchronization and lifetimes](NATIVE_GPU_SYNCHRONIZATION_AND_LIFETIMES_LAB.md), [Vulkan partial-construction rollback](VULKAN_PARTIAL_CONSTRUCTION_ROLLBACK_LAB.md), and [GPU upload/renderer teardown](https://github.com/swayam8624/KairoGameEngine/blob/main/docs/GPU_RESOURCE_ROLLBACK_AND_RENDERER_TEARDOWN_LAB.md). Unlike a source-only comment pass, this chapter accompanies a **functional change to `VulkanRendererRuntime::DrawFrame`** and a GPU-independent regression test.

Read [`VulkanRenderer.cppm`](../VulkanRenderer.cppm), [`detail/VulkanFrameRecoveryJournal.hpp`](../detail/VulkanFrameRecoveryJournal.hpp), [`tests/VulkanFrameRecoveryJournalTests.cpp`](../tests/VulkanFrameRecoveryJournalTests.cpp), [`VulkanSync.cppm`](../VulkanSync.cppm), [`VulkanSwapchain.cppm`](../VulkanSwapchain.cppm) and [`VulkanCommand.cppm`](../VulkanCommand.cppm). These files together establish the frame contract; none is sufficient by itself.

**Verification boundary:** The checked-in C++23 state-journal regression target models failure transitions without a Vulkan device. Locally, the two corresponding algorithms were compiled and exercised as a small independent CPU check with GCC 14 and Clang 17. The exact full-project native Vulkan runtime, WSI recovery, queue-error fault injection and validation-layer behavior have **not** been demonstrated by that CPU check.

## 1. A Vulkan frame is not a function call

A successful renderer call can contain five separate operations:

```text
CPU retire previous command buffer
    vkWaitForFences(InFlight)

WSI acquire
    vkAcquireNextImageKHR(swapchain, ImageAvailable, ..., &imageIndex)

CPU record
    vkResetCommandBuffer
    vkBeginCommandBuffer ... vkCmd* ... vkEndCommandBuffer

Queue submission
    vkResetFences(InFlight)
    vkQueueSubmit(
        wait=ImageAvailable,
        signal=RenderFinished[imageIndex],
        fence=InFlight)

Presentation
    vkQueuePresentKHR(wait=RenderFinished[imageIndex], imageIndex)
```

Each arrow means **a different native event**. Acquiring an image is not rendering it. Ending CPU command recording is not submitting GPU work. Successful submission is not GPU completion. Enqueuing presentation is not the same as host fence completion. The CPU `RenderGraph` orders the callbacks but cannot replace the Vulkan synchronization operations.

## 2. The original deadlock sequence

The earlier `VulkanRendererRuntime::DrawFrame` reset the in-flight fence **inside the Acquire callback**, immediately after `vkAcquireNextImageKHR` returned success or suboptimal.

If the subsequent `VulkanTriangle::Record` failed—for example, due to a material descriptor allocation exception or command recording error—no `vkQueueSubmit` operation would associate with that reset fence. On the next `DrawFrame`, the Retire callback waited with `vkWaitForFences(..., UINT64_MAX)`. Nothing in the queue was obligated to signal the fence.

This is a classical lost-signal deadlock. **Waiting longer cannot fix it.**

The first repair is to **move `vkResetFences` to the Submit callback after successful command recording**. This removes the recording-failure → unsignaled-fence path. However, `vkQueueSubmit` itself can still fail after the fence was reset, and a successful acquire leaves a separate WSI semaphore/image state to resolve.

## 3. A small explicit recovery journal

The CPU-only [`VulkanFrameRecoveryJournal`](../detail/VulkanFrameRecoveryJournal.hpp) records only facts established by successful native calls:

| Phase | Last successful native milestone | Acquired image outstanding? | ImageAvailable consumed by successful draw submit? |
| --- | --- | --- | --- |
| `BeforeAcquire` | None | No | No |
| `Acquired` | `vkAcquireNextImageKHR` | Yes | No |
| `FenceReset` | `vkResetFences` | Yes | No |
| `Submitted` | `vkQueueSubmit` | Yes, until presentation | Yes |
| `Completed` | Success or handled presentation/rebuild | No current recovery obligation | Yes |

The journal refuses illegal transitions such as presenting without submission or resetting the frame fence before an acquire. It also separates `AcquireSemaphoreUnconsumed()` from `FenceResetWithoutSubmission()`. These describe **different hazards**.

The journal has no Vulkan headers or handles; a mistaken journal cannot itself invalidate a `VkDevice`. It is deliberately a testable policy model, not a pretend GPU fence.

## 4. Why an acquire semaphore must be consumed

The WSI acquire call signals `ImageAvailable` **when** the acquired image is available for graphics, not necessarily at the moment the CPU function returns. On the normal path, `vkQueueSubmit` supplies this semaphore in `pWaitSemaphores`; the queue wait synchronizes drawing against presentation-engine use and consumes the binary semaphore signal.

Suppose an image was acquired, but command recording threw **before any queue submission**. The next frame must not simply call `vkAcquireNextImageKHR` with the same already-pending or signaled binary semaphore. Likewise, the renderer must not assume that `vkDeviceWaitIdle` *alone* always proves the unconsumed WSI acquire-signal operation is finished, because that signal comes from image acquisition rather than ordinary submitted graphics commands.

The repaired runtime uses a **synchronization-only graphics submission** when the journal is `Acquired` or `FenceReset`:

```cpp
VkSubmitInfo drain{};
drain.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
drain.waitSemaphoreCount = 1;
drain.pWaitSemaphores = &imageAvailable;
drain.pWaitDstStageMask = &topOfPipeStage;
drain.commandBufferCount = 0;   // A queue wait; no graphics command buffers

vkQueueSubmit(graphicsQueue, 1, &drain, VK_NULL_HANDLE);
```

The following `RecreateSwapchain` calls `vkDeviceWaitIdle` before the old sync objects and swapchain are destroyed. That idle wait now includes the queue wait submitted to consume `ImageAvailable`. If the drain submit itself fails, the renderer is marked unusable rather than pretending that old semaphore ownership can safely be reused.

The Vulkan specification permits `commandBufferCount == 0` in a `VkSubmitInfo`, while nonzero `waitSemaphoreCount` requires valid wait semaphores and stage-mask entries. See the official [VkSubmitInfo reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkSubmitInfo.html), [vkQueueSubmit reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html), and [WSI acquire contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkAcquireNextImageKHR.html).

## 5. Four failure classes and their recovery policy

| Failure point | Journal condition | Current action |
| --- | --- | --- |
| Host graph compile / previous fence wait fails before image acquisition | `BeforeAcquire` | Propagate exception, mark renderer unusable when native state is uncertain; no arbitrary image-drain submission |
| Image acquired, then descriptor/recording failure | `Acquired` | Submit a wait-only drain of ImageAvailable; idle GPU; replace swapchain/synchronization; clear abandoned pick/capture |
| Fence reset then `vkQueueSubmit` returns failure | `FenceReset` | Try drain of original acquired semaphore; idle/recreate full generation; never wait on the old unsignaled fence |
| Graphics submit succeeds, then `vkQueuePresentKHR` fails | `Submitted` | ImageAvailable has already been waited; idle and recreate without a second acquire drain |

A `VK_ERROR_OUT_OF_DATE_KHR` on **acquire** means no image was acquired: the existing resize path recreates and skips the frame. An out-of-date or suboptimal **present** is handled after submission by the existing resize logic, and the journal records the completed handoff only after that reconstruction succeeds.

On an exception, the runtime preserves the original error for the caller. It temporarily sets `m_FrameUsable = false`; only an entirely successful rebuild restores the flag. If idling or reconstruction fails (as can happen on device loss), future `DrawFrame` calls throw **immediately**, avoiding infinite waits on an unsignaled fence or reusing old WSI semaphores.

This is **best-effort recovery of a frame**, not a full device-loss recovery implementation. A destroyed/failed Vulkan device may still need application-level renderer reconstruction. The renderer does not claim to restore the failed frame's content; pending pick/capture requests and in-flight result flags are discarded after successful recovery.

## 6. What the CPU regression actually checks

[`VulkanFrameRecoveryJournalTests.cpp`](../tests/VulkanFrameRecoveryJournalTests.cpp) exercises:

1. No acquired image: no arbitrary swapchain recovery and illegal early submission rejected.
2. Acquire then record failure: outstanding image and unconsumed ImageAvailable detected; fence remains signaled.
3. Fence reset then failed submit: unsignaled-fence hazard and unconsumed acquire semaphore both detected.
4. Submit accepted but present failure: GPU/WSI retirement needed, but ImageAvailable already consumed by submission.
5. Normal acquire → reset → submit → present: no extra recreation required.
6. Illegal state transitions: second acquire, early completion and submission without fence reset rejected.

The new CTest name is `KairoRendererVulkanFrameRecoveryJournalTests`. It is a **header-only policy test**; it cannot exercise real `vkQueueSubmit` validation, driver queue semantics or image lifetime.

With sibling repositories checked out, [the KairoGameEngine standalone runner](https://github.com/swayam8624/KairoGameEngine/blob/main/scripts/run_renderer_resource_lifetime_regressions.sh) compiles four pure CPU regressions (Player ownership, native resource publication, Vulkan frame journal, material file size budget) on a C++23 compiler.

## 7. Native tests still required

Use a Vulkan validation-enabled build with a real presentable surface, a correctly configured GPU driver, and the repo's exact commit hash:

1. Force a deterministic failure **after successful acquire but before command buffer Begin**. Verify a synchronization-only queue wait runs, the old swapchain retires, and a subsequent good frame renders.
2. Force a failure **inside command recording**, including after a render pass begins. Verify command-buffer state and target destruction/recreation satisfy Vulkan validation constraints.
3. Inject `vkQueueSubmit` failure after fence reset. If recovery is possible, confirm old unsignaled fence is never waited and resources are reconstructed; if device loss occurs, confirm subsequent frames fail immediately.
4. Force `vkQueuePresentKHR` out-of-date/suboptimal/fatal results. Confirm submitted command completion, present semaphore/image ownership and generation switches are correct.
5. Repeat minimization, resize, acquire out-of-date and recovery in the same process. Track semaphore/fence/image-view lifetimes through validation and GPU-assisted checks where available.
6. Exercise pending object-ID pick and HDR capture during each failure. They must not return results from commands that never completed.
7. Run the official [swapchain semaphore reuse guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html) against the chosen one-frame/per-image semaphore policy; generic fence retirement is not a substitute for proof of presentation completion.

**Critical limitation:** a successful CPU regression and specification-based reasoning are not evidence that every Vulkan WSI driver accepts the recovery implementation under all error modes. Device-loss semantics and native resource destruction after an unrecoverable queue failure remain separate design/test work.

## 8. Concluding model: three independently retired things

A failed frame can leave three separate obligations: a WSI **acquired image**, an **ImageAvailable binary semaphore signal**, and a **CPU fence reset without a successful queue submission**. The recovery journal tells the runtime which obligations exist. The native backend must use queue synchronization and full resource-generation retirement to satisfy them.

This is why merely moving `vkResetFences` later, or catching an exception and calling `vkDeviceWaitIdle`, is not by itself a complete frame-recovery design.
