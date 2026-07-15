module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.VulkanSync;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: a logical Vulkan device.
    /// Output: acquire, per-swapchain-image render-complete, and CPU/GPU fence
    /// objects for one frame in flight.
    /// Task: make the one-buffer renderer race-free before introducing a
    /// multi-frame scheduler. The fence begins signaled so the first frame can
    /// record immediately.
    class VulkanFrameSync final
    {
    public:
        VulkanFrameSync(const VulkanDevice& device, std::uint32_t swapchainImageCount)
            : m_Device(device.Handle())
        {
            Create(swapchainImageCount);
        }

        ~VulkanFrameSync() { Destroy(); }
        VulkanFrameSync(const VulkanFrameSync&) = delete;
        VulkanFrameSync& operator=(const VulkanFrameSync&) = delete;

        /// Precondition: the device is idle. This is satisfied by swapchain
        /// recreation before image-count-dependent semaphores are released.
        void Recreate(std::uint32_t swapchainImageCount)
        {
            Destroy();
            Create(swapchainImageCount);
        }

        [[nodiscard]] VkSemaphore ImageAvailable() const noexcept { return m_ImageAvailable; }
        [[nodiscard]] VkSemaphore RenderFinished(std::uint32_t imageIndex) const { return m_RenderFinished.at(imageIndex); }
        [[nodiscard]] VkFence InFlight() const noexcept { return m_InFlight; }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkSemaphore m_ImageAvailable = VK_NULL_HANDLE;
        std::vector<VkSemaphore> m_RenderFinished;
        VkFence m_InFlight = VK_NULL_HANDLE;

        void Create(std::uint32_t swapchainImageCount)
        {
            if (swapchainImageCount == 0u)
            {
                throw std::invalid_argument("VulkanFrameSync requires at least one swapchain image.");
            }
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            if (vkCreateSemaphore(m_Device, &semaphore, nullptr, &m_ImageAvailable) != VK_SUCCESS ||
                vkCreateFence(m_Device, &fence, nullptr, &m_InFlight) != VK_SUCCESS)
            {
                throw std::runtime_error("Vulkan frame synchronization creation failed.");
            }
            try
            {
                m_RenderFinished.resize(swapchainImageCount, VK_NULL_HANDLE);
                for (VkSemaphore& renderFinished : m_RenderFinished)
                {
                    if (vkCreateSemaphore(m_Device, &semaphore, nullptr, &renderFinished) != VK_SUCCESS)
                    {
                        throw std::runtime_error("Vulkan render-finished semaphore creation failed.");
                    }
                }
            }
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        void Destroy() noexcept
        {
            if (m_InFlight != VK_NULL_HANDLE) vkDestroyFence(m_Device, m_InFlight, nullptr);
            m_InFlight = VK_NULL_HANDLE;
            for (const VkSemaphore renderFinished : m_RenderFinished)
            {
                if (renderFinished != VK_NULL_HANDLE) vkDestroySemaphore(m_Device, renderFinished, nullptr);
            }
            m_RenderFinished.clear();
            if (m_ImageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(m_Device, m_ImageAvailable, nullptr);
            m_ImageAvailable = VK_NULL_HANDLE;
        }
    };
}
