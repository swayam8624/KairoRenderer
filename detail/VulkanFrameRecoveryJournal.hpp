#pragma once

// CPU-only model of the single-frame Vulkan WSI lifecycle. The renderer
// invokes each transition after the corresponding Vulkan operation succeeds.
// VulkanFrameRecoveryJournal never owns a native VkFence, VkSemaphore or image;
// it only answers whether an acquired image / reset fence / queued submission
// must be retired through a swapchain rebuild after a failed frame.
//
// Kept independent of vulkan.h so failure-state tests can compile on any
// C++23 host without a Vulkan SDK, loader, GLFW window or presentable GPU.

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>

namespace kairo::renderer::detail
{
    enum class VulkanFramePhase : std::uint8_t
    {
        BeforeAcquire,
        Acquired,    // WSI image + ImageAvailable semaphore were acquired
        FenceReset,  // CPU fence unsignaled, no successful submit yet
        Submitted,   // vkQueueSubmit accepted work; presentation still pending
        Completed    // vkQueuePresentKHR accepted or swapchain handled/rebuilt
    };

    class VulkanFrameRecoveryJournal final
    {
        VulkanFramePhase m_Phase = VulkanFramePhase::BeforeAcquire;

    public:
        [[nodiscard]] VulkanFramePhase Phase() const noexcept
        {
            return m_Phase;
        }

        // A successful vkAcquireNextImageKHR returns an acquired image and
        // a binary ImageAvailable signal. If later recording fails, a fresh
        // swapchain/synchronization generation is needed before reacquiring.
        void Acquired()
        {
            if (m_Phase != VulkanFramePhase::BeforeAcquire)
                throw std::logic_error("Vulkan frame image acquisition is out of order.");
            m_Phase = VulkanFramePhase::Acquired;
        }

        // Reset the frame fence only after command recording has succeeded.
        // A failure after reset but before successful submit leaves no queue
        // operation obligated to signal the fence. Never wait it again
        // without first rebuilding the synchronization generation.
        void FenceReset()
        {
            if (m_Phase != VulkanFramePhase::Acquired)
                throw std::logic_error("Vulkan frame fence reset is out of order.");
            m_Phase = VulkanFramePhase::FenceReset;
        }

        void Submitted()
        {
            if (m_Phase != VulkanFramePhase::FenceReset)
                throw std::logic_error("Vulkan frame submission is out of order.");
            m_Phase = VulkanFramePhase::Submitted;
        }

        // Call only after a successful presentation, or after the native
        // swapchain recreation has handled an out-of-date/suboptimal result.
        void Completed()
        {
            if (m_Phase != VulkanFramePhase::Submitted)
                throw std::logic_error("Vulkan presentation finished without submission.");
            m_Phase = VulkanFramePhase::Completed;
        }

        [[nodiscard]] bool HasAcquiredImage() const noexcept
        {
            return m_Phase == VulkanFramePhase::Acquired ||
                m_Phase == VulkanFramePhase::FenceReset ||
                m_Phase == VulkanFramePhase::Submitted;
        }

        [[nodiscard]] bool AcquireSemaphoreUnconsumed() const noexcept
        {
            // A successful real vkQueueSubmit waits on ImageAvailable.
            // Recording failure / submit failure leaves that binary WSI
            // signal without a queue wait. A synchronization-only submission
            // must consume it before its semaphore/swapchain is destroyed.
            return m_Phase == VulkanFramePhase::Acquired ||
                m_Phase == VulkanFramePhase::FenceReset;
        }

        [[nodiscard]] bool FenceResetWithoutSubmission() const noexcept
        {
            return m_Phase == VulkanFramePhase::FenceReset;
        }

        [[nodiscard]] bool HasPendingPresentation() const noexcept
        {
            return m_Phase == VulkanFramePhase::Submitted;
        }

        [[nodiscard]] bool RequiresSwapchainRecovery() const noexcept
        {
            return HasAcquiredImage();
        }
    };

    // An ordinary host exception BEFORE any Vulkan image acquisition does not
    // invalidate a signaled frame fence or a binary WSI semaphore. Allow the
    // caller to retry such a CPU-only failure. Device errors during the Retire
    // fence wait or vkAcquireNextImageKHR, and incomplete swapchain rebuilds,
    // still require a sticky unusable state rather than blind retries.
    [[nodiscard]] inline bool CanRetryBeforeVulkanAcquire(
        const VulkanFrameRecoveryJournal& frame,
        bool rebuildingAlreadyInProgress,
        bool nativeOperationFailed) noexcept
    {
        return frame.Phase() == VulkanFramePhase::BeforeAcquire &&
            !rebuildingAlreadyInProgress && !nativeOperationFailed;
    }

    // This is the SAME failure-recovery decision called by VulkanRenderer.
    // Native work is supplied as callbacks: the test can inject failures and
    // assert ordering without pretending to submit actual GPU commands.
    // Only a complete drain (if needed) AND successful generation rebuild
    // permit the caller to clear its sticky unusable-renderer flag.
    template<class DrainAcquire, class RebuildGeneration>
    [[nodiscard]] bool TryRecoverFailedVulkanFrame(
        const VulkanFrameRecoveryJournal& frame,
        bool rebuildingAlreadyInProgress,
        DrainAcquire&& drainAcquire,
        RebuildGeneration&& rebuildGeneration) noexcept
    {
        if (rebuildingAlreadyInProgress ||
            !frame.RequiresSwapchainRecovery())
            return false;
        try
        {
            if (frame.AcquireSemaphoreUnconsumed())
                std::invoke(std::forward<DrainAcquire>(drainAcquire));
            std::invoke(std::forward<RebuildGeneration>(rebuildGeneration));
            return true;
        }
        catch (...)
        {
            // Preserve sticky invalid state when draining/rebuilding fails.
            return false;
        }
    }
}
