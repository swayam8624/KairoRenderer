module;

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

export module Kairo.Renderer.VulkanBuffer;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: device, byte count, and Vulkan buffer usage flags.
    /// Output: host-visible, host-coherent buffer storage mapped for its entire
    /// lifetime.
    /// Task: provide deterministic V1 mesh/uniform upload. Device-local staging
    /// buffers are intentionally deferred until the draw contract is proven.
    class VulkanHostBuffer final
    {
    public:
        VulkanHostBuffer(const VulkanDevice& device, VkDeviceSize bytes, VkBufferUsageFlags usage)
            : m_Device(device.Handle()), m_Size(bytes)
        {
            if (bytes == 0u)
            {
                throw std::invalid_argument("VulkanHostBuffer requires a non-zero byte count.");
            }
            VkBufferCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            create.size = bytes;
            create.usage = usage;
            create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(m_Device, &create, nullptr, &m_Buffer) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateBuffer failed.");
            }
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(m_Device, m_Buffer, &requirements);
            VkMemoryAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = FindMemoryType(device.PhysicalHandle(), requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(m_Device, &allocate, nullptr, &m_Memory) != VK_SUCCESS)
            {
                throw std::runtime_error("vkAllocateMemory for a host buffer failed.");
            }
            if (vkBindBufferMemory(m_Device, m_Buffer, m_Memory, 0u) != VK_SUCCESS ||
                vkMapMemory(m_Device, m_Memory, 0u, bytes, 0u, &m_Mapped) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to bind or map a Vulkan host buffer.");
            }
        }

        ~VulkanHostBuffer()
        {
            if (m_Mapped != nullptr) vkUnmapMemory(m_Device, m_Memory);
            if (m_Memory != VK_NULL_HANDLE) vkFreeMemory(m_Device, m_Memory, nullptr);
            if (m_Buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_Device, m_Buffer, nullptr);
        }

        VulkanHostBuffer(const VulkanHostBuffer&) = delete;
        VulkanHostBuffer& operator=(const VulkanHostBuffer&) = delete;
        [[nodiscard]] VkBuffer Handle() const noexcept { return m_Buffer; }

        /// Precondition: byteCount does not exceed construction size.
        /// Task: copy CPU bytes into coherent GPU-visible storage.
        void Write(const void* source, VkDeviceSize byteCount) const
        {
            if (source == nullptr || byteCount > m_Size)
            {
                throw std::invalid_argument("VulkanHostBuffer write exceeds its mapped storage.");
            }
            std::memcpy(m_Mapped, source, static_cast<std::size_t>(byteCount));
        }

        /// Input: writable destination and requested byte count.
        /// Output: a snapshot of host-coherent GPU-written storage.
        /// Precondition: the queue operation writing this buffer has completed.
        void Read(void* destination, VkDeviceSize byteCount) const
        {
            if (destination == nullptr || byteCount > m_Size)
                throw std::invalid_argument("VulkanHostBuffer read exceeds its mapped storage.");
            std::memcpy(destination, m_Mapped, static_cast<std::size_t>(byteCount));
        }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkBuffer m_Buffer = VK_NULL_HANDLE;
        VkDeviceMemory m_Memory = VK_NULL_HANDLE;
        void* m_Mapped = nullptr;
        VkDeviceSize m_Size = 0u;

        [[nodiscard]] static std::uint32_t FindMemoryType(VkPhysicalDevice physical, std::uint32_t bits, VkMemoryPropertyFlags required)
        {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physical, &properties);
            for (std::uint32_t index = 0u; index < properties.memoryTypeCount; ++index)
            {
                if ((bits & (1u << index)) != 0u &&
                    (properties.memoryTypes[index].propertyFlags & required) == required)
                {
                    return index;
                }
            }
            throw std::runtime_error("No Vulkan memory type satisfies the requested host buffer properties.");
        }
    };
}
