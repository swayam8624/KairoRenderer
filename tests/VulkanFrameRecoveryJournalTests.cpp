// GPU-independent lifecycle regression for the exact frame-state journal
// called by VulkanRendererRuntime::DrawFrame. It models successful acquire,
// recording failure, fence reset, queue-submit failure and presentation.
#include "../detail/VulkanFrameRecoveryJournal.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    template<class Callable>
    void RequireLogicError(Callable&& operation, const char* message)
    {
        bool threw = false;
        try { operation(); }
        catch (const std::logic_error&) { threw = true; }
        Require(threw, message);
    }
}

int main()
{
    try
    {
        using kairo::renderer::detail::VulkanFrameRecoveryJournal;
        using kairo::renderer::detail::VulkanFramePhase;

        // An exception before vkAcquireNextImageKHR succeeds must not
        // cause arbitrary semaphore/fence replacement in the recovery path.
        VulkanFrameRecoveryJournal untouched;
        Require(untouched.Phase() == VulkanFramePhase::BeforeAcquire &&
            !untouched.RequiresSwapchainRecovery() &&
            !untouched.FenceResetWithoutSubmission(),
            "Pre-acquisition execution should need no image recovery");
        RequireLogicError([&] { untouched.FenceReset(); },
            "Fence reset before image acquisition was allowed");
        RequireLogicError([&] { untouched.Submitted(); },
            "Submission without a reset frame fence was allowed");
        RequireLogicError([&] { untouched.Completed(); },
            "Presentation without submission was allowed");

        // Acquire succeeds, recording then throws. ImageAvailable may have
        // been signaled, but the frame fence is STILL SIGNALED because it
        // must not be reset until recording finishes. Recreate swapchain and
        // synchronization generation before trying to acquire again.
        VulkanFrameRecoveryJournal recordFailed;
        recordFailed.Acquired();
        Require(recordFailed.HasAcquiredImage() &&
            recordFailed.RequiresSwapchainRecovery() &&
            !recordFailed.FenceResetWithoutSubmission(),
            "Recording failure did not require acquired-image recovery");
        RequireLogicError([&] { recordFailed.Acquired(); },
            "Second image acquire without retirement was allowed");

        // Reset the fence only after successful command recording. A failed
        // vkQueueSubmit leaves no queued work responsible for signaling it:
        // attempting the next frame's infinite vkWaitForFences would deadlock.
        VulkanFrameRecoveryJournal submitFailed;
        submitFailed.Acquired();
        submitFailed.FenceReset();
        Require(submitFailed.FenceResetWithoutSubmission() &&
            submitFailed.RequiresSwapchainRecovery(),
            "Unsignaled fence with failed submit was not identified");
        RequireLogicError([&] { submitFailed.Completed(); },
            "A frame with no successful submission was marked presented");

        // Submission accepted, but vkQueuePresentKHR then fails. Recovery
        // must wait the in-flight GPU work and release acquired WSI state.
        VulkanFrameRecoveryJournal presentFailed;
        presentFailed.Acquired();
        presentFailed.FenceReset();
        presentFailed.Submitted();
        Require(presentFailed.HasPendingPresentation() &&
            presentFailed.RequiresSwapchainRecovery() &&
            !presentFailed.FenceResetWithoutSubmission(),
            "Failed presentation did not request safe retirement");

        // Normal successful submission/presentation is complete. This frame
        // must never rebuild the swapchain merely because it once acquired.
        VulkanFrameRecoveryJournal completed;
        completed.Acquired();
        completed.FenceReset();
        completed.Submitted();
        completed.Completed();
        Require(completed.Phase() == VulkanFramePhase::Completed &&
            !completed.HasAcquiredImage() &&
            !completed.HasPendingPresentation() &&
            !completed.FenceResetWithoutSubmission() &&
            !completed.RequiresSwapchainRecovery(),
            "Successful frame incorrectly requests recovery");
        RequireLogicError([&] { completed.Acquired(); },
            "Completed single-frame journal unexpectedly reused");

        std::puts("KairoRenderer Vulkan frame recovery journal tests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan frame recovery regression: %s\n", error.what());
        return 1;
    }
}
