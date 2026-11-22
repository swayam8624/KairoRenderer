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
#include <stdexcept>

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
}
