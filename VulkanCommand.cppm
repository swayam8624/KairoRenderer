module;

#include <vulkan/vulkan.h>

#include <stdexcept>

// TEACHING VULKAN OWNERSHIP: a primary VkCommandBuffer belongs to a
// VkCommandPool which belongs to the VkDevice/graphics queue family.
// VulkanRenderer::DrawFrame waits its frame fence before Begin(); a graph
// pass declaration alone cannot establish GPU recording safety.
export module Kairo.Renderer.VulkanCommand;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: a logical device with a graphics queue family.
    /// Output: one reusable primary command buffer.
    /// Task: centralize command-pool ownership for the single-frame renderer
    /// baseline. The buffer is reset before every frame and is not safe for
    /// concurrent recording; multi-frame command ownership is a later policy.
    class VulkanCommandBuffer final
    {
    public:
        // Create a RESET_COMMAND_BUFFER-capable command pool for the queue
        // family selected by VulkanDevice, then allocate ONE primary buffer.
        // AUDIT: if allocation throws after pool creation, the class
        // destructor does not run for the incomplete constructor; a local
        // guard is needed for robust constructor-failure cleanup.
        explicit VulkanCommandBuffer(const VulkanDevice& device)
            : m_Device(device.Handle())
        {
            VkCommandPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = device.GraphicsFamily();
            if (vkCreateCommandPool(m_Device, &pool, nullptr, &m_Pool) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateCommandPool failed.");
            }

            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = m_Pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1u;
            if (vkAllocateCommandBuffers(m_Device, &allocate, &m_Handle) != VK_SUCCESS)
            {
                throw std::runtime_error("vkAllocateCommandBuffers failed.");
            }
        }

        // Destroying the pool implicitly releases its command buffers.
        // Caller must ensure all submitted GPU uses have completed and
        // VulkanDevice outlives this object; VulkanRenderer owns that order.
        ~VulkanCommandBuffer()
        {
            if (m_Pool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_Device, m_Pool, nullptr);
            }
        }

        VulkanCommandBuffer(const VulkanCommandBuffer&) = delete;
        VulkanCommandBuffer& operator=(const VulkanCommandBuffer&) = delete;

        [[nodiscard]] VkCommandBuffer Handle() const noexcept { return m_Handle; }

        /// Precondition: GPU work submitted with this buffer has completed.
        /// Task: reset and begin recording the next primary command sequence.
        // Reset and begin recording the single reusable command buffer.
        // This is only legal AFTER the previous submitted work has signaled
        // the VkFence; VulkanRenderer Retire/Acquire owns that precondition.
        void Begin() const
        {
            if (vkResetCommandBuffer(m_Handle, 0u) != VK_SUCCESS)
            {
                throw std::runtime_error("vkResetCommandBuffer failed.");
            }
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (vkBeginCommandBuffer(m_Handle, &begin) != VK_SUCCESS)
            {
                throw std::runtime_error("vkBeginCommandBuffer failed.");
            }
        }

        /// Task: finalize the recorded commands for queue submission.
        // Finish host command recording, not GPU execution. The recorded
        // buffer remains pending after vkQueueSubmit until the fence signals.
        void End() const
        {
            if (vkEndCommandBuffer(m_Handle) != VK_SUCCESS)
            {
                throw std::runtime_error("vkEndCommandBuffer failed.");
            }
        }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkCommandPool m_Pool = VK_NULL_HANDLE;
        VkCommandBuffer m_Handle = VK_NULL_HANDLE;
    };
}
