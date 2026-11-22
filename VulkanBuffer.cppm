module;

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

// TEACHING VULKAN BUFFER: VulkanHostBuffer owns VkBuffer + VkDeviceMemory
// + a persistent CPU mapping. HOST_VISIBLE|HOST_COHERENT means CPU writes
// do not need explicit vkFlushMappedMemoryRanges; it does NOT replace
// queue semaphore/fence synchronization for racing GPU reads/writes.
export module Kairo.Renderer.VulkanBuffer;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: device, byte count, and Vulkan buffer usage flags.
    /// Output: host-visible, host-coherent buffer storage mapped for its entire
    /// lifetime.
    /// Task: provide deterministic V1 mesh/uniform upload. Device-local staging
    /// buffers are intentionally deferred until the draw contract is proven.
    // Transfers ownership of a host-coherent staging allocation to the
    // parent VulkanDevice only when a submitted upload's retirement cannot
    // be proven. No Vulkan destroy happens during this transfer. The mapped
    // pointer must be unmapped before the memory is freed at later retirement.
    struct DetachedVulkanHostBuffer final
    {
        VkBuffer Buffer = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        void* Mapped = nullptr;
    };

    class VulkanHostBuffer final
    {
    public:
        // Create VkBuffer, query its memory requirements, choose a memory
        // type satisfying required host visibility/coherence, allocate,
        // bind and map the requested byte range. Device is borrowed.
        // Constructor catches any later allocation/bind/map error and calls
        // the same partial-handle-safe Destroy used by the destructor. This
        // matters because the destructor does not run for a failed constructor.
        VulkanHostBuffer(const VulkanDevice& device, VkDeviceSize bytes, VkBufferUsageFlags usage)
            : m_Device(device.Handle()), m_Size(bytes)
        {
            if (bytes == 0u)
            {
                throw std::invalid_argument("VulkanHostBuffer requires a non-zero byte count.");
            }
            try
            {
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
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        // Unmap, destroy buffer, then free device memory after all GPU work
        // referencing it retires. Persistent mapping does not pin a CPU
        // pointer beyond this object's lifetime.
        ~VulkanHostBuffer() { Destroy(); }

        VulkanHostBuffer(const VulkanHostBuffer&) = delete;
        VulkanHostBuffer& operator=(const VulkanHostBuffer&) = delete;
        [[nodiscard]] VkBuffer Handle() const noexcept { return m_Buffer; }

        // Called only on an unproven GPU completion: the original owner
        // must NOT destroy pending VkBuffer/VkDeviceMemory in its destructor.
        // The device-level deferred journal assumes responsibility for
        // unmap -> buffer destroy -> memory free after idle/device-loss.
        [[nodiscard]] DetachedVulkanHostBuffer DetachForDeferredRetirement() noexcept
        {
            const DetachedVulkanHostBuffer handles{ m_Buffer, m_Memory, m_Mapped };
            m_Buffer = VK_NULL_HANDLE;
            m_Memory = VK_NULL_HANDLE;
            m_Mapped = nullptr;
            m_Size = 0u;
            return handles;
        }

        /// Precondition: byteCount does not exceed construction size.
        /// Task: copy CPU bytes into coherent GPU-visible storage.
        // memcpy into mapped host-coherent storage. Guard source and size,
        // but callers must ensure no simultaneous GPU read of overwritten
        // bytes; CPU memory coherence is not execution-order synchronization.
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
        // memcpy after the GPU writer has signaled completion (typically
        // the frame fence). The mapped address alone does not guarantee
        // the producer command has finished or the layout is valid.
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

        // A single noexcept rollback path handles a partially constructed
        // buffer, a failed map, and normal destruction. The VkBuffer must be
        // destroyed before freeing the VkDeviceMemory bound to it.
        void Destroy() noexcept
        {
            if (m_Mapped != nullptr)
                vkUnmapMemory(m_Device, m_Memory);
            m_Mapped = nullptr;
            if (m_Buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(m_Device, m_Buffer, nullptr);
            m_Buffer = VK_NULL_HANDLE;
            if (m_Memory != VK_NULL_HANDLE)
                vkFreeMemory(m_Device, m_Memory, nullptr);
            m_Memory = VK_NULL_HANDLE;
            m_Size = 0u;
        }

        // Select a physical-device memoryTypeIndex compatible with the
        // buffer requirements bitmask AND host-visible/coherent properties.
        // Failure means this simple host-buffer strategy is unavailable,
        // not that every possible Vulkan staging strategy is unavailable.
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
