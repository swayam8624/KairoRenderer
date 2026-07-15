module;

#include <cstdint>
#include <vulkan/vulkan.h>

#include <array>
#include <stdexcept>

export module Kairo.Renderer.VulkanDescriptor;

import Kairo.Renderer.VulkanBuffer;
import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: a Vulkan uniform buffer and a sampled directional shadow map.
    /// Output: descriptor-set layout, pool, and one set bound at set=0,
    /// binding=0 for the full buffer range and binding=1 for the depth texture.
    /// Task: keep frame-global descriptor ownership out of pipeline recording.
    /// The descriptor does not own the buffer, image view, or sampler; those
    /// resources must outlive this object.
    class VulkanUniformDescriptor final
    {
    public:
        VulkanUniformDescriptor(const VulkanDevice& device, const VulkanHostBuffer& buffer, VkDeviceSize range,
            VkImageView shadowView, VkSampler shadowSampler)
            : m_Device(device.Handle())
        {
            if (range == 0u)
            {
                throw std::invalid_argument("VulkanUniformDescriptor requires a non-zero uniform range.");
            }
            if (shadowView == VK_NULL_HANDLE || shadowSampler == VK_NULL_HANDLE)
                throw std::invalid_argument("VulkanUniformDescriptor requires a valid shadow view and sampler.");

            try
            {
                const std::array bindings{
                    VkDescriptorSetLayoutBinding{ 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                    VkDescriptorSetLayoutBinding{ 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr }
                };
                VkDescriptorSetLayoutCreateInfo layout{};
                layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
                layout.pBindings = bindings.data();
                if (vkCreateDescriptorSetLayout(m_Device, &layout, nullptr, &m_Layout) != VK_SUCCESS)
                    throw std::runtime_error("vkCreateDescriptorSetLayout failed.");

                const std::array poolSizes{
                    VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
                    VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u }
                };
                VkDescriptorPoolCreateInfo pool{};
                pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                pool.maxSets = 1u;
                pool.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
                pool.pPoolSizes = poolSizes.data();
                if (vkCreateDescriptorPool(m_Device, &pool, nullptr, &m_Pool) != VK_SUCCESS)
                    throw std::runtime_error("vkCreateDescriptorPool failed.");

                VkDescriptorSetAllocateInfo allocation{};
                allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocation.descriptorPool = m_Pool;
                allocation.descriptorSetCount = 1u;
                allocation.pSetLayouts = &m_Layout;
                if (vkAllocateDescriptorSets(m_Device, &allocation, &m_Set) != VK_SUCCESS)
                    throw std::runtime_error("vkAllocateDescriptorSets failed.");

                const VkDescriptorBufferInfo bufferInfo{ buffer.Handle(), 0u, range };
                const VkDescriptorImageInfo imageInfo{
                    shadowSampler, shadowView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                };
                std::array<VkWriteDescriptorSet, 2> writes{};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = m_Set;
                writes[0].dstBinding = 0u;
                writes[0].descriptorCount = 1u;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].pBufferInfo = &bufferInfo;
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = m_Set;
                writes[1].dstBinding = 1u;
                writes[1].descriptorCount = 1u;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo = &imageInfo;
                vkUpdateDescriptorSets(m_Device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
            }
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        ~VulkanUniformDescriptor()
        {
            Destroy();
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

        void Destroy() noexcept
        {
            if (m_Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
            if (m_Layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
            m_Set = VK_NULL_HANDLE;
            m_Pool = VK_NULL_HANDLE;
            m_Layout = VK_NULL_HANDLE;
        }
    };
}
