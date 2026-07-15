module;

#include <vulkan/vulkan.h>

#include <stdexcept>

export module Kairo.Renderer.VulkanDescriptor;

import Kairo.Renderer.VulkanBuffer;
import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: a Vulkan uniform buffer.
    /// Output: descriptor-set layout, pool, and one set bound at set=0,
    /// binding=0 for the full buffer range.
    /// Task: keep descriptor ownership local to the resource that publishes a
    /// per-frame uniform block. Texture/sampler descriptors will extend this
    /// pattern instead of embedding raw allocation calls in each pipeline.
    class VulkanUniformDescriptor final
    {
    public:
        VulkanUniformDescriptor(const VulkanDevice& device, const VulkanHostBuffer& buffer, VkDeviceSize range)
            : m_Device(device.Handle())
        {
            if (range == 0u)
            {
                throw std::invalid_argument("VulkanUniformDescriptor requires a non-zero uniform range.");
            }
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0u;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding.descriptorCount = 1u;
            binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo layout{};
            layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout.bindingCount = 1u;
            layout.pBindings = &binding;
            if (vkCreateDescriptorSetLayout(m_Device, &layout, nullptr, &m_Layout) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateDescriptorSetLayout failed.");
            }
            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            poolSize.descriptorCount = 1u;
            VkDescriptorPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets = 1u;
            pool.poolSizeCount = 1u;
            pool.pPoolSizes = &poolSize;
            if (vkCreateDescriptorPool(m_Device, &pool, nullptr, &m_Pool) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateDescriptorPool failed.");
            }
            VkDescriptorSetAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocation.descriptorPool = m_Pool;
            allocation.descriptorSetCount = 1u;
            allocation.pSetLayouts = &m_Layout;
            if (vkAllocateDescriptorSets(m_Device, &allocation, &m_Set) != VK_SUCCESS)
            {
                throw std::runtime_error("vkAllocateDescriptorSets failed.");
            }
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = buffer.Handle();
            bufferInfo.range = range;
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_Set;
            write.dstBinding = 0u;
            write.descriptorCount = 1u;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(m_Device, 1u, &write, 0u, nullptr);
        }

        ~VulkanUniformDescriptor()
        {
            if (m_Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
            if (m_Layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
        }

        VulkanUniformDescriptor(const VulkanUniformDescriptor&) = delete;
        VulkanUniformDescriptor& operator=(const VulkanUniformDescriptor&) = delete;

        [[nodiscard]] VkDescriptorSetLayout Layout() const noexcept { return m_Layout; }
        [[nodiscard]] VkDescriptorSet Set() const noexcept { return m_Set; }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;
        VkDescriptorSet m_Set = VK_NULL_HANDLE;
    };
}
