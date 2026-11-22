// GPU-independent lifecycle regression for the exact frame-state journal
// called by VulkanRendererRuntime::DrawFrame. It models successful acquire,
// recording failure, fence reset, queue-submit failure and presentation.
#include "../detail/VulkanFrameRecoveryJournal.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

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
            !untouched.FenceResetWithoutSubmission() &&
            !untouched.AcquireSemaphoreUnconsumed(),
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
            recordFailed.AcquireSemaphoreUnconsumed() &&
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
            submitFailed.RequiresSwapchainRecovery() &&
            submitFailed.AcquireSemaphoreUnconsumed(),
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
            !presentFailed.AcquireSemaphoreUnconsumed() &&
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
            !completed.AcquireSemaphoreUnconsumed() &&
            !completed.FenceResetWithoutSubmission() &&
            !completed.RequiresSwapchainRecovery(),
            "Successful frame incorrectly requests recovery");
        RequireLogicError([&] { completed.Acquired(); },
            "Completed single-frame journal unexpectedly reused");

        // Execute the very same recovery helper used by the native runtime.
        // A recording failure MUST consume ImageAvailable before swapchain
        // retirement, while a submitted-but-unpresented frame must not drain
        // the already-consumed binary semaphore a second time.
        using kairo::renderer::detail::TryRecoverFailedVulkanFrame;
        std::vector<std::string> operations;
        const auto drain = [&] { operations.emplace_back("drain"); };
        const auto rebuild = [&] { operations.emplace_back("rebuild"); };

        Require(TryRecoverFailedVulkanFrame(recordFailed, false,
            drain, rebuild) &&
            operations == std::vector<std::string>{ "drain", "rebuild" },
            "Recording failure did not drain then rebuild");

        operations.clear();
        Require(TryRecoverFailedVulkanFrame(submitFailed, false,
            drain, rebuild) &&
            operations == std::vector<std::string>{ "drain", "rebuild" },
            "Submit failure did not drain and replace the unsignaled fence");

        operations.clear();
        Require(TryRecoverFailedVulkanFrame(presentFailed, false,
            drain, rebuild) &&
            operations == std::vector<std::string>{ "rebuild" },
            "Present failure incorrectly drained an already-waited semaphore");

        operations.clear();
        Require(!TryRecoverFailedVulkanFrame(untouched, false,
            drain, rebuild) && operations.empty(),
            "Pre-acquisition error triggered a native rebuild");
        Require(!TryRecoverFailedVulkanFrame(recordFailed, true,
            drain, rebuild) && operations.empty(),
            "Nested swapchain-rebuild failure attempted a second rebuild");

        operations.clear();
        const bool drainFailed = TryRecoverFailedVulkanFrame(recordFailed, false,
            [&] {
                operations.emplace_back("drain");
                throw std::runtime_error("injected acquire drain failure");
            },
            rebuild);
        Require(!drainFailed && operations ==
            std::vector<std::string>{ "drain" },
            "A failed acquire drain incorrectly proceeded to resource teardown");

        operations.clear();
        const bool rebuildFailed = TryRecoverFailedVulkanFrame(recordFailed, false,
            drain,
            [&] {
                operations.emplace_back("rebuild");
                throw std::runtime_error("injected native rebuild failure");
            });
        Require(!rebuildFailed && operations ==
            std::vector<std::string>{ "drain", "rebuild" },
            "An unsuccessful native rebuild was incorrectly treated as safe");

        // A C++ RenderGraph compilation/profiling exception with no WSI
        // acquisition (or after successful present) may safely be retried.
        // Native fence/acquire errors must NOT be interpreted as host-only.
        using kairo::renderer::detail::CanRetryWithoutVulkanRebuild;
        Require(CanRetryWithoutVulkanRebuild(untouched, false, false),
            "A pre-acquire CPU-only exception unnecessarily poisoned the device");
        Require(CanRetryWithoutVulkanRebuild(completed, false, false),
            "A post-present CPU-only exception unnecessarily poisoned the device");
        Require(!CanRetryWithoutVulkanRebuild(untouched, false, true) &&
            !CanRetryWithoutVulkanRebuild(untouched, true, false) &&
            !CanRetryWithoutVulkanRebuild(recordFailed, false, false) &&
            !CanRetryWithoutVulkanRebuild(presentFailed, false, false),
            "Unsafe Vulkan state was incorrectly marked safe for host retry");

        std::puts("KairoRenderer Vulkan frame recovery journal tests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan frame recovery regression: %s\n", error.what());
        return 1;
    }
}
