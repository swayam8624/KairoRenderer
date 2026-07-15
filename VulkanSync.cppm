module;

#include <vulkan/vulkan.h>

#include <stdexcept>

export module Kairo.Renderer.VulkanSync;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: a logical Vulkan device.
    /// Output: the acquire, render-complete, and CPU/GPU fence objects for one
    /// frame in flight.
    /// Task: make the one-buffer renderer race-free before introducing a
    /// multi-frame scheduler. The fence begins signaled so the first frame can
    /// record immediately.
    class VulkanFrameSync final
    {
    public:
        explicit VulkanFrameSync(const VulkanDevice& device)
            : m_Device(device.Handle())
        {
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            if (vkCreateSemaphore(m_Device, &semaphore, nullptr, &m_ImageAvailable) != VK_SUCCESS ||
                vkCreateSemaphore(m_Device, &semaphore, nullptr, &m_RenderFinished) != VK_SUCCESS ||
                vkCreateFence(m_Device, &fence, nullptr, &m_InFlight) != VK_SUCCESS)
            {
                throw std::runtime_error("Vulkan frame synchronization creation failed.");
            }
        }

        ~VulkanFrameSync()
        {
            if (m_InFlight != VK_NULL_HANDLE) vkDestroyFence(m_Device, m_InFlight, nullptr);
            if (m_RenderFinished != VK_NULL_HANDLE) vkDestroySemaphore(m_Device, m_RenderFinished, nullptr);
            if (m_ImageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(m_Device, m_ImageAvailable, nullptr);
        }

        VulkanFrameSync(const VulkanFrameSync&) = delete;
        VulkanFrameSync& operator=(const VulkanFrameSync&) = delete;

        [[nodiscard]] VkSemaphore ImageAvailable() const noexcept { return m_ImageAvailable; }
        [[nodiscard]] VkSemaphore RenderFinished() const noexcept { return m_RenderFinished; }
        [[nodiscard]] VkFence InFlight() const noexcept { return m_InFlight; }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkSemaphore m_ImageAvailable = VK_NULL_HANDLE;
        VkSemaphore m_RenderFinished = VK_NULL_HANDLE;
        VkFence m_InFlight = VK_NULL_HANDLE;
    };
}
