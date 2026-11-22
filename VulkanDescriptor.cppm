module;

#include <cstdint>
#include <vulkan/vulkan.h>

#include <array>
#include <bit>
#include <cstddef>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include "detail/TransactionalResourceGeneration.hpp"

// TEACHING DESCRIPTORS: VkDescriptorSetLayout declares shader-visible
// bindings, VkDescriptorPool allocates set storage, and each VkDescriptorSet
// BORROWS the referenced image views, samplers and uniform buffers.
// VulkanTriangle builds per-draw bindings only after frame-fence retirement.
// The descriptor's declared image layout must match actual native barriers.
export module Kairo.Renderer.VulkanDescriptor;

import Kairo.Renderer.VulkanBuffer;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.Material;
import Kairo.Renderer.Skinning;

export namespace kairo::renderer
{
    /// Input: a Vulkan uniform buffer and a sampled directional shadow map.
    /// Output: descriptor-set layout, pool, and one set bound at set=0,
    /// binding=0 for the full buffer range and binding=1 for the depth texture.
    /// Task: keep frame-global descriptor ownership out of pipeline recording.
    /// The descriptor does not own the buffer, image view, or sampler; those
    /// resources must outlive this object.
    // One global camera/shadow set: binding 0 uniform buffer, binding 1
    // sampled depth shadow texture. No ownership of either referenced input.
    class VulkanUniformDescriptor final
    {
    public:
        // Allocate stable layout+pool+set and write two borrowed bindings.
        // The constructor catches mid-build failure and destroys owned Vulkan
        // descriptor objects; it never frees the caller's uniform/shadow GPU
        // memory. Resource lifetime must exceed all in-flight draws.
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
    // Per-draw INPUT bundle of five borrowed PBR image/sampler pairs, a
    // borrowed environment image, a validated CPU material, and optionally
    // a borrowed SkinPalette. Rebuild copies numeric skin data into owned UBOs.
    struct VulkanMaterialImages final
    {
        std::array<VkImageView, 5u> Views{};
        std::array<VkSampler, 5u> Samplers{};
        VkImageView EnvironmentView = VK_NULL_HANDLE;
        VkSampler EnvironmentSampler = VK_NULL_HANDLE;
        PBRMaterial Material;
        /// Optional per-draw joint palette. The descriptor owner copies it into
        /// fence-safe UBO storage during Rebuild; callers retain no GPU lifetime.
        const SkinPalette* Skinning = nullptr;
    };

    /// Owns a stable descriptor-set layout and a fence-safe, rebuildable pool
    /// of per-draw material sets. Camera and shadow resources are shared; each
    /// set owns a small coherent material uniform and five sampled channels.
    // Ten-slot shader interface: 0 camera, 1 directional shadow, 2-6 five
    // PBR samplers, 7 material uniform, 8 environment, 9 optional skin UBO.
    // The descriptor-set layout persists; the set pool and uniform buffers
    // are rebuilt per scene version after prior frame work completes.
    class VulkanMaterialDescriptors final
    {
    public:
        // Declare set layout ONCE with fixed descriptor types and shader
        // visibility. VulkanTriangle::BindMaterial relies on this same layout;
        // adding a binding requires a shader and pipeline-layout update.
        explicit VulkanMaterialDescriptors(const VulkanDevice& device)
            : m_DeviceObject(device), m_Device(device.Handle()),
              m_Generation(device.Handle())
        {
            std::array<VkDescriptorSetLayoutBinding, 10u> bindings{};
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
            bindings[9] = { 9u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr };
            VkDescriptorSetLayoutCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            create.bindingCount = static_cast<std::uint32_t>(bindings.size());
            create.pBindings = bindings.data();
            if (vkCreateDescriptorSetLayout(m_Device, &create, nullptr, &m_Layout) != VK_SUCCESS)
                throw std::runtime_error("vkCreateDescriptorSetLayout for PBR materials failed.");
        }

        ~VulkanMaterialDescriptors()
        {
            // Release the current descriptor generation BEFORE its shared
            // VkDescriptorSetLayout. Generation::~Generation also handles
            // partially staged candidate resource rollback on exceptions.
            m_Generation.Clear();
            if (m_Layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
        }
        VulkanMaterialDescriptors(const VulkanMaterialDescriptors&) = delete;
        VulkanMaterialDescriptors& operator=(const VulkanMaterialDescriptors&) = delete;

        [[nodiscard]] VkDescriptorSetLayout Layout() const noexcept { return m_Layout; }
        [[nodiscard]] std::size_t Size() const noexcept { return m_Generation.Sets.size(); }
        // Return a borrowed descriptor set indexed by current draw order.
        // Rebuild can invalidate the handle and reorder per-draw bindings;
        // never cache it across scene descriptor regeneration.
        [[nodiscard]] VkDescriptorSet Set(std::size_t index) const
        {
            if (index >= m_Generation.Sets.size())
                throw std::out_of_range("Material descriptor index is outside the current render scene.");
            return m_Generation.Sets[index];
        }

        /// Precondition: the render fence has completed and every supplied
        /// image remains alive until the next rebuild. Rebuild is intentionally
        /// frame-boundary work; descriptor pools are never destroyed in flight.
        // Validate and construct a detached descriptor pool, sets, and UBOs
        // before publishing. All failures, including native allocation and
        // C++ bad_alloc, leave the prior generation unmodified. Only the
        // fence-retired caller may retire the previous published pool.
        void Rebuild(const VulkanHostBuffer& camera, VkDeviceSize cameraRange,
            VkImageView shadowView, VkSampler shadowSampler,
            std::span<const VulkanMaterialImages> materials)
        {
            // The old generation is NEVER mutated while a candidate is being
            // validated or allocated. A staged owner's destructor releases all
            // partial VkDescriptorPool / VulkanHostBuffer allocations on throw.
            // The no-throw swap publishes only a fully finished generation.
            // The previous generation retires when staged leaves this scope.
            // PRECONDITION: all previously submitted draws are fence-retired.
            DescriptorGeneration staged(m_Device);
            detail::BuildAndCommitResourceGeneration(m_Generation, staged,
                [&](DescriptorGeneration& candidate)
                {
                    // An empty scene is a valid new generation with no sets.
                    if (materials.empty()) return;
                    if (cameraRange == 0u || shadowView == VK_NULL_HANDLE ||
                        shadowSampler == VK_NULL_HANDLE)
                        throw std::invalid_argument("Material descriptors require valid camera and shadow resources.");
                    if (materials.size() >
                        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() / 7u))
                        throw std::length_error("Material descriptor pool count would overflow Vulkan uint32 capacities.");
                    for (const auto& source : materials)
                    {
                        source.Material.Validate();
                        for (std::size_t channel = 0u; channel < 5u; ++channel)
                            if (source.Views[channel] == VK_NULL_HANDLE ||
                                source.Samplers[channel] == VK_NULL_HANDLE)
                                throw std::invalid_argument("PBR material descriptors require five valid sampled images.");
                        if (source.EnvironmentView == VK_NULL_HANDLE ||
                            source.EnvironmentSampler == VK_NULL_HANDLE)
                            throw std::invalid_argument("PBR material descriptors require a valid environment fallback image.");
                        if (source.Skinning != nullptr && !source.Skinning->Empty())
                            source.Skinning->Validate();
                    }

                const std::uint32_t count = static_cast<std::uint32_t>(materials.size());
                const std::array poolSizes{
                    VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count * 3u },
                    VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * 7u }
                };
                VkDescriptorPoolCreateInfo pool{};
                pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                pool.maxSets = count;
                pool.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
                pool.pPoolSizes = poolSizes.data();
                if (vkCreateDescriptorPool(m_Device, &pool, nullptr, &candidate.Pool) != VK_SUCCESS)
                    throw std::runtime_error("vkCreateDescriptorPool for PBR materials failed.");

                std::vector<VkDescriptorSetLayout> layouts(count, m_Layout);
                candidate.Sets.resize(count);
                VkDescriptorSetAllocateInfo allocation{};
                allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocation.descriptorPool = candidate.Pool;
                allocation.descriptorSetCount = count;
                allocation.pSetLayouts = layouts.data();
                if (vkAllocateDescriptorSets(m_Device, &allocation, candidate.Sets.data()) != VK_SUCCESS)
                {
                    throw std::runtime_error("vkAllocateDescriptorSets for PBR materials failed.");
                }

                candidate.MaterialBuffers.reserve(count);
                candidate.SkinBuffers.reserve(count);
                for (std::uint32_t index = 0u; index < count; ++index)
                {
                    // All source factor/handle/palette validation completed
                    // before the old descriptor pool was retired. No repeated
                    // per-draw validation is necessary during native creation.
                    const auto& source = materials[index];

                    // Pack material factors in the exact shader uniform slot
                    // sequence: base RGBA, emissive RGB/normal scale, metallic,
                    // roughness, AO, alpha threshold/mode and double-sided bit.
                    // The temporary CPU array is copied into a per-draw UBO.
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
                    // The five sampled views must already be in native
                    // SHADER_READ_ONLY_OPTIMAL layout, established by the texture
                    // uploader or a later explicit render-pass transition.
                    std::array<VkDescriptorImageInfo, 5u> imageInfos{};
                    for (std::size_t channel = 0u; channel < imageInfos.size(); ++channel)
                        imageInfos[channel] = { source.Samplers[channel], source.Views[channel],
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    const VkDescriptorBufferInfo materialInfo{ buffer->Handle(), 0u,
                        sizeof(MaterialUniform) };
                    const VkDescriptorImageInfo environmentInfo{ source.EnvironmentSampler,
                        source.EnvironmentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

                    // Copy the caller-owned SkinPalette into a new persistent
                    // host-coherent uniform buffer. Vulkan shaders read a stable
                    // GPU allocation, never the transient CPU palette pointer.
                    std::unique_ptr<VulkanHostBuffer> skinBuffer;
                    VkDescriptorBufferInfo skinInfo{};
                    if (source.Skinning != nullptr && !source.Skinning->Empty())
                    {
                        SkinUniform skin{};
                        for (std::size_t joint = 0u; joint < source.Skinning->Size(); ++joint)
                            for (std::size_t row = 0u; row < 4u; ++row)
                                for (std::size_t column = 0u; column < 4u; ++column)
                                    skin.Values[joint * 16u + column * 4u + row] =
                                        source.Skinning->JointMatrices[joint](row, column);
                        skinBuffer = std::make_unique<VulkanHostBuffer>(m_DeviceObject,
                            sizeof(SkinUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
                        skinBuffer->Write(&skin, sizeof(skin));
                        skinInfo = { skinBuffer->Handle(), 0u, sizeof(SkinUniform) };
                    }

                    // DescriptorWrite arrays hold pointers to local VkDescriptor*
                    // structs ONLY until vkUpdateDescriptorSets copies the values.
                    // The referenced VkBuffer/VkImageView/VkSampler handles remain
                    // borrowed and require longer GPU lifetime management.
                    std::array<VkWriteDescriptorSet, 10u> writes{};
                    writes[0] = Write(candidate.Sets[index], 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                    writes[0].pBufferInfo = &cameraInfo;
                    writes[1] = Write(candidate.Sets[index], 1u,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                    writes[1].pImageInfo = &shadowInfo;
                    for (std::uint32_t channel = 0u; channel < 5u; ++channel)
                    {
                        writes[2u + channel] = Write(candidate.Sets[index], 2u + channel,
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                        writes[2u + channel].pImageInfo = &imageInfos[channel];
                    }
                    writes[7] = Write(candidate.Sets[index], 7u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                    writes[7].pBufferInfo = &materialInfo;
                    writes[8] = Write(candidate.Sets[index], 8u,
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                    writes[8].pImageInfo = &environmentInfo;
                    std::uint32_t writeCount = 9u;
                    if (skinBuffer)
                    {
                        writes[9] = Write(candidate.Sets[index], 9u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                        writes[9].pBufferInfo = &skinInfo;
                        writeCount = 10u;
                    }
                    vkUpdateDescriptorSets(m_Device, writeCount, writes.data(), 0u, nullptr);
                    candidate.MaterialBuffers.push_back(std::move(buffer));
                    candidate.SkinBuffers.push_back(std::move(skinBuffer));
                }
                });
        }

    private:
        struct MaterialUniform final { std::array<float, 16u> Values{}; };
        struct SkinUniform final
        {
            std::array<float, MaximumSkinJoints * 16u> Values{};
        };
        const VulkanDevice& m_DeviceObject;
        VkDevice m_Device = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
        // A complete and independent descriptor generation. Candidate
        // allocation never writes into the current live resources; the old
        // generation moves into staged on a noexcept swap and is destroyed
        // only after successful publication at a retired frame boundary.
        struct DescriptorGeneration final
        {
            explicit DescriptorGeneration(VkDevice device) noexcept
                : Device(device) {}
            ~DescriptorGeneration() { Clear(); }
            DescriptorGeneration(const DescriptorGeneration&) = delete;
            DescriptorGeneration& operator=(const DescriptorGeneration&) = delete;

            VkDevice Device = VK_NULL_HANDLE;
            VkDescriptorPool Pool = VK_NULL_HANDLE;
            std::vector<VkDescriptorSet> Sets;
            std::vector<std::unique_ptr<VulkanHostBuffer>> MaterialBuffers;
            std::vector<std::unique_ptr<VulkanHostBuffer>> SkinBuffers;

            void Clear() noexcept
            {
                SkinBuffers.clear();
                MaterialBuffers.clear();
                Sets.clear();
                if (Pool != VK_NULL_HANDLE)
                    vkDestroyDescriptorPool(Device, Pool, nullptr);
                Pool = VK_NULL_HANDLE;
            }

            void Swap(DescriptorGeneration& other) noexcept
            {
                // Both generations borrow the SAME live VkDevice. Swapping
                // numeric pool ownership + allocator-compatible STL vectors
                // requires no allocation, so publication cannot fail.
                std::swap(Pool, other.Pool);
                Sets.swap(other.Sets);
                MaterialBuffers.swap(other.MaterialBuffers);
                SkinBuffers.swap(other.SkinBuffers);
            }
        };

        DescriptorGeneration m_Generation;

        // Fill one descriptor-write header with a type, target set and slot;
        // callers add the matching buffer/image pointer before the update.
        [[nodiscard]] static VkWriteDescriptorSet Write(VkDescriptorSet set,
            std::uint32_t binding, VkDescriptorType type) noexcept
        {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = 1u;
            write.descriptorType = type;
            return write;
        }

    };
}
