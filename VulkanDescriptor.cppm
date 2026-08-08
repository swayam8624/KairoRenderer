module;

#include <cstdint>
#include <vulkan/vulkan.h>

#include <array>
#include <bit>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.VulkanDescriptor;

import Kairo.Renderer.VulkanBuffer;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.Material;

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

    /// Non-owning texture views used to assemble one per-draw material set.
    /// Order is base color, normal, metallic-roughness, emissive, occlusion.
    struct VulkanMaterialImages final
    {
        std::array<VkImageView, 5u> Views{};
        std::array<VkSampler, 5u> Samplers{};
        VkImageView EnvironmentView = VK_NULL_HANDLE;
        VkSampler EnvironmentSampler = VK_NULL_HANDLE;
        PBRMaterial Material;
    };

    /// Owns a stable descriptor-set layout and a fence-safe, rebuildable pool
    /// of per-draw material sets. Camera and shadow resources are shared; each
    /// set owns a small coherent material uniform and five sampled channels.
    class VulkanMaterialDescriptors final
    {
    public:
        explicit VulkanMaterialDescriptors(const VulkanDevice& device)
            : m_DeviceObject(device), m_Device(device.Handle())
        {
            std::array<VkDescriptorSetLayoutBinding, 9u> bindings{};
            bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            for (std::uint32_t binding = 1u; binding <= 6u; ++binding)
                bindings[binding] = { binding,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            bindings[7] = { 7u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            bindings[8] = { 8u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            VkDescriptorSetLayoutCreateInfo create{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            create.bindingCount = static_cast<std::uint32_t>(bindings.size());
            create.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(m_Device, &create, nullptr, &m_Layout) != VK_SUCCESS)
                throw std::runtime_error("vkCreateDescriptorSetLayout for PBR materials failed.");
        }

        ~VulkanMaterialDescriptors()
        {
            ClearPool();
            if (m_Layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
        }
        VulkanMaterialDescriptors(const VulkanMaterialDescriptors&) = delete;
        VulkanMaterialDescriptors& operator=(const VulkanMaterialDescriptors&) = delete;

        [[nodiscard]] VkDescriptorSetLayout Layout() const noexcept { return m_Layout; }
        [[nodiscard]] std::size_t Size() const noexcept { return m_Sets.size(); }
        [[nodiscard]] VkDescriptorSet Set(std::size_t index) const
        {
            if (index >= m_Sets.size())
                throw std::out_of_range("Material descriptor index is outside the current render scene.");
            return m_Sets[index];
        }

        /// Precondition: the render fence has completed and every supplied
        /// image remains alive until the next rebuild. Rebuild is intentionally
        /// frame-boundary work; descriptor pools are never destroyed in flight.
        void Rebuild(const VulkanHostBuffer& camera, VkDeviceSize cameraRange,
            VkImageView shadowView, VkSampler shadowSampler,
            std::span<const VulkanMaterialImages> materials)
        {
            ClearPool();
            if (materials.empty()) return;
            if (cameraRange == 0u || shadowView == VK_NULL_HANDLE ||
                shadowSampler == VK_NULL_HANDLE)
                throw std::invalid_argument("Material descriptors require valid camera and shadow resources.");

            const std::uint32_t count = static_cast<std::uint32_t>(materials.size());
            const std::array poolSizes{
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count * 2u },
                VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * 7u }
            };
            VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pool.maxSets = count;
            pool.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
            pool.pPoolSizes = poolSizes.data();
            if (vkCreateDescriptorPool(m_Device, &pool, nullptr, &m_Pool) != VK_SUCCESS)
                throw std::runtime_error("vkCreateDescriptorPool for PBR materials failed.");

            std::vector<VkDescriptorSetLayout> layouts(count, m_Layout);
            m_Sets.resize(count);
            VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocation.descriptorPool = m_Pool;
            allocation.descriptorSetCount = count;
            allocation.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(m_Device, &allocation, m_Sets.data()) != VK_SUCCESS)
            {
                ClearPool();
                throw std::runtime_error("vkAllocateDescriptorSets for PBR materials failed.");
            }

            m_MaterialBuffers.reserve(count);
            for (std::uint32_t index = 0u; index < count; ++index)
            {
                const auto& source = materials[index];
                source.Material.Validate();
                for (std::size_t channel = 0u; channel < 5u; ++channel)
                    if (source.Views[channel] == VK_NULL_HANDLE ||
                        source.Samplers[channel] == VK_NULL_HANDLE)
                        throw std::invalid_argument("PBR material descriptors require five valid sampled images.");
                if (source.EnvironmentView == VK_NULL_HANDLE ||
                    source.EnvironmentSampler == VK_NULL_HANDLE)
                    throw std::invalid_argument(
                        "PBR material descriptors require a valid environment fallback image.");

                MaterialUniform uniform{};
                uniform.Values[0] = source.Material.BaseColor.x;
                uniform.Values[1] = source.Material.BaseColor.y;
                uniform.Values[2] = source.Material.BaseColor.z;
                uniform.Values[3] = source.Material.BaseColorAlpha;
                uniform.Values[4] = source.Material.Emissive.x;
                uniform.Values[5] = source.Material.Emissive.y;
                uniform.Values[6] = source.Material.Emissive.z;
                uniform.Values[7] = source.Material.NormalScale;
                uniform.Values[8] = source.Material.Metallic;
                uniform.Values[9] = source.Material.Roughness;
                uniform.Values[10] = source.Material.AmbientOcclusion;
                uniform.Values[11] = source.Material.AlphaCutoff;
                uniform.Values[12] = static_cast<float>(source.Material.AlphaMode);
                uniform.Values[13] = source.Material.DoubleSided ? 1.0f : 0.0f;
                auto buffer = std::make_unique<VulkanHostBuffer>(m_DeviceObject,
                    sizeof(MaterialUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
                buffer->Write(&uniform, sizeof(uniform));

                const VkDescriptorBufferInfo cameraInfo{ camera.Handle(), 0u, cameraRange };
                const VkDescriptorImageInfo shadowInfo{ shadowSampler, shadowView,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
                std::array<VkDescriptorImageInfo, 5u> imageInfos{};
                for (std::size_t channel = 0u; channel < imageInfos.size(); ++channel)
                    imageInfos[channel] = { source.Samplers[channel], source.Views[channel],
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                const VkDescriptorBufferInfo materialInfo{ buffer->Handle(), 0u,
                    sizeof(MaterialUniform) };
                const VkDescriptorImageInfo environmentInfo{ source.EnvironmentSampler,
                    source.EnvironmentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                std::array<VkWriteDescriptorSet, 9u> writes{};
                writes[0] = Write(m_Sets[index], 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                writes[0].pBufferInfo = &cameraInfo;
                writes[1] = Write(m_Sets[index], 1u,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                writes[1].pImageInfo = &shadowInfo;
                for (std::uint32_t channel = 0u; channel < 5u; ++channel)
                {
                    writes[2u + channel] = Write(m_Sets[index], 2u + channel,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                    writes[2u + channel].pImageInfo = &imageInfos[channel];
                }
                writes[7] = Write(m_Sets[index], 7u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                writes[7].pBufferInfo = &materialInfo;
                writes[8] = Write(m_Sets[index], 8u,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                writes[8].pImageInfo = &environmentInfo;
                vkUpdateDescriptorSets(m_Device,
                    static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);
                m_MaterialBuffers.push_back(std::move(buffer));
            }
        }

    private:
        struct MaterialUniform final { std::array<float, 16u> Values{}; };
        const VulkanDevice& m_DeviceObject;
        VkDevice m_Device = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> m_Sets;
        std::vector<std::unique_ptr<VulkanHostBuffer>> m_MaterialBuffers;

        [[nodiscard]] static VkWriteDescriptorSet Write(VkDescriptorSet set,
            std::uint32_t binding, VkDescriptorType type) noexcept
        {
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = 1u;
            write.descriptorType = type;
            return write;
        }

        void ClearPool() noexcept
        {
            m_MaterialBuffers.clear();
            m_Sets.clear();
            if (m_Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
            m_Pool = VK_NULL_HANDLE;
        }
    };
}
