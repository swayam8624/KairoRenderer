module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include "detail/TextureMipUploadPlan.hpp"
#include "detail/VulkanUploadRetirementPolicy.hpp"

// TEACHING UPLOAD: validated KairoAssets TextureArtifactData -> host-coherent
// Vulkan staging bytes -> device-local VkImage mips -> sampled image view and
// sampler. This module performs REAL native barriers, unlike logical
// RenderGraph transitions. Its native resources are owned by VulkanTexture2D
// and borrowed by VulkanMaterialDescriptors. See TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md.
export module Kairo.Renderer.VulkanTexture;

import Kairo.Assets.TextureArtifact;
import Kairo.Renderer.Texture;
import Kairo.Renderer.VulkanBuffer;
import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Owning Vulkan sampled image built from the canonical KairoAssets texture
    /// artifact. Input mip bytes are copied exactly once through a host staging
    /// buffer; the resulting image is device-local and shader-read-only.
    // VkImage+VkDeviceMemory+VkImageView+VkSampler all belong to this one
    // move-disabled RAII value. VulkanTriangle holds it via unique_ptr;
    // descriptor bindings borrow View()/Sampler() without taking ownership.
    class VulkanTexture2D final
    {
    public:
        // Validate authored pixels/mips FIRST. Then select a device format,
        // allocate image+view+sampler and synchronously upload all mip bytes.
        // The explicit try/catch invokes Destroy if native construction fails
        // partway through: unlike a default C++ destructor, this covers
        // partially built VulkanTexture2D instances.
        VulkanTexture2D(const VulkanDevice& device,
            const kairo::assets::TextureArtifactData& texture,
            TextureSampler sampling = {})
            : m_Device(device.Handle())
        {
            device.CheckUsable();
            kairo::assets::ValidateTextureArtifactData(texture);
            m_Format = SelectFormat(texture);
            m_Width = texture.Mips.front().Width;
            m_Height = texture.Mips.front().Height;
            m_MipLevels = static_cast<std::uint32_t>(texture.Mips.size());
            try
            {
                CreateImage(device);
                CreateView();
                CreateSampler(sampling);
                Upload(device, texture);
            }
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        ~VulkanTexture2D() { Destroy(); }
        VulkanTexture2D(const VulkanTexture2D&) = delete;
        VulkanTexture2D& operator=(const VulkanTexture2D&) = delete;

        [[nodiscard]] VkImageView View() const noexcept { return m_View; }
        [[nodiscard]] VkSampler Sampler() const noexcept { return m_Sampler; }
        [[nodiscard]] std::uint32_t Width() const noexcept { return m_Width; }
        [[nodiscard]] std::uint32_t Height() const noexcept { return m_Height; }
        [[nodiscard]] std::uint32_t MipLevels() const noexcept { return m_MipLevels; }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkImage m_Image = VK_NULL_HANDLE;
        VkDeviceMemory m_Memory = VK_NULL_HANDLE;
        VkImageView m_View = VK_NULL_HANDLE;
        VkSampler m_Sampler = VK_NULL_HANDLE;
        VkFormat m_Format = VK_FORMAT_UNDEFINED;
        std::uint32_t m_Width = 0u;
        std::uint32_t m_Height = 0u;
        std::uint32_t m_MipLevels = 0u;

        // Preserve the asset's color-space semantics: sRGB for encoded color,
        // UNORM for linear sampled channels, and float16 where requested.
        // Applying sRGB interpretation to normal/metallic channels is a
        // data bug even when the Vulkan image dimensions are valid.
        [[nodiscard]] static VkFormat SelectFormat(
            const kairo::assets::TextureArtifactData& texture)
        {
            using enum kairo::assets::TexturePixelFormat;
            switch (texture.Format)
            {
                case R8: return texture.ColorSpace == kairo::assets::TextureColorSpace::SRGB
                    ? VK_FORMAT_R8_SRGB : VK_FORMAT_R8_UNORM;
                case RG8: return texture.ColorSpace == kairo::assets::TextureColorSpace::SRGB
                    ? VK_FORMAT_R8G8_SRGB : VK_FORMAT_R8G8_UNORM;
                case RGBA8: return texture.ColorSpace == kairo::assets::TextureColorSpace::SRGB
                    ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                case RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
            }
            throw std::invalid_argument("Texture artifact has no Vulkan format mapping.");
        }

        // Intersect VkImage memoryTypeBits with the physical device's
        // supported DEVICE_LOCAL properties. A format can be supported yet
        // the chosen allocation requirements still lack a compatible type.
        [[nodiscard]] static std::uint32_t FindMemoryType(VkPhysicalDevice physical,
            std::uint32_t bits, VkMemoryPropertyFlags required)
        {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physical, &properties);
            for (std::uint32_t index = 0u; index < properties.memoryTypeCount; ++index)
                if ((bits & (1u << index)) != 0u &&
                    (properties.memoryTypes[index].propertyFlags & required) == required)
                    return index;
            throw std::runtime_error("No Vulkan memory type supports a device-local texture.");
        }

        // Allocate a 2D optimal-tiling VkImage with all artifact mip levels,
        // device-local storage and TRANSFER_DST|SAMPLED usage. Initial layout
        // is UNDEFINED; content becomes valid only after Upload barriers/copy.
        void CreateImage(const VulkanDevice& device)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device.PhysicalHandle(), m_Format, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0u)
                throw std::runtime_error("Selected Vulkan device cannot sample the texture artifact format.");

            VkImageCreateInfo create{};

            create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            create.imageType = VK_IMAGE_TYPE_2D;
            create.format = m_Format;
            create.extent = { m_Width, m_Height, 1u };
            create.mipLevels = m_MipLevels;
            create.arrayLayers = 1u;
            create.samples = VK_SAMPLE_COUNT_1_BIT;
            create.tiling = VK_IMAGE_TILING_OPTIMAL;
            create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_Device, &create, nullptr, &m_Image) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImage for texture failed.");

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_Device, m_Image, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = FindMemoryType(device.PhysicalHandle(),
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(m_Device, &allocation, nullptr, &m_Memory) != VK_SUCCESS)
                throw std::runtime_error("vkAllocateMemory for texture failed.");
            if (vkBindImageMemory(m_Device, m_Image, m_Memory, 0u) != VK_SUCCESS)
                throw std::runtime_error("vkBindImageMemory for texture failed.");
        }

        // Create one sampled color-aspect image view spanning ALL mip levels.
        // This is a view/format contract; it does not transfer or decode bytes.
        void CreateView()
        {
            VkImageViewCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            create.image = m_Image;
            create.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create.format = m_Format;
            create.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 1u };
            if (vkCreateImageView(m_Device, &create, nullptr, &m_View) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImageView for texture failed.");
        }

        // Map canonical address modes to Vulkan repeat/mirror/clamp enums.
        // Invalid enum values fail rather than silently changing UV behavior.
        [[nodiscard]] static VkSamplerAddressMode Address(TextureAddressMode mode)
        {
            switch (mode)
            {
                case TextureAddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case TextureAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case TextureAddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
            throw std::invalid_argument("Texture address mode is invalid.");
        }

        // Request linear min/mag and trilinear mip interpolation with LOD
        // clamped to the authored mip-count-1. AddressU/V follow the asset;
        // W is clamped. Anisotropy is not enabled by this simple sampler.
        void CreateSampler(TextureSampler sampling)
        {
            VkSamplerCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            create.magFilter = VK_FILTER_LINEAR;
            create.minFilter = VK_FILTER_LINEAR;
            create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            create.addressModeU = Address(sampling.AddressU);
            create.addressModeV = Address(sampling.AddressV);
            create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            create.minLod = 0.0f;
            create.maxLod = static_cast<float>(m_MipLevels - 1u);
            create.maxAnisotropy = 1.0f;
            if (vkCreateSampler(m_Device, &create, nullptr, &m_Sampler) != VK_SUCCESS)
                throw std::runtime_error("vkCreateSampler for texture failed.");
        }

        // Derive contiguous upload bytes plus explicit per-mip offsets through
        // the same GPU-independent helper exercised by the C++23 regressions.
        // Record one transient command buffer with two VkImageMemoryBarriers:
        // UNDEFINED -> TRANSFER_DST_OPTIMAL, copy each VkBufferImageCopy mip,
        // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL. Wait for the entire
        // graphics queue to idle before staging buffer/pool destruction.
        // The production SubmitAndRetireVulkanUpload helper decides whether
        // queue completion was proven. VK_ERROR_DEVICE_LOST is retirement for
        // the Vulkan resource-in-use rule; uncertain other errors transfer
        // ALL staging, command and texture ownership to VulkanDevice's
        // deferred journal instead of destroying pending native allocations.
        void Upload(const VulkanDevice& device,
            const kairo::assets::TextureArtifactData& texture)
        {
            // KairoAssets checked canonical mip dimensions and exact bytes.
            // This extra gate rejects overflow before VkDeviceSize conversion
            // and keeps the byte offsets tested independently from Vulkan.
            const auto upload = detail::MakeTextureMipUploadPlan(texture.Mips);
            // Pre-reserve the failure journal before any GPU submit; an
            // allocation exception while trying to remember pending native
            // work would otherwise recreate the very failure we are fixing.
            device.ReserveDeferredTextureUploadSlot();
            VulkanHostBuffer staging(device,
                static_cast<VkDeviceSize>(upload.PackedBytes.size()),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            staging.Write(upload.PackedBytes.data(),
                static_cast<VkDeviceSize>(upload.PackedBytes.size()));

            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer command = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo poolCreate{};
            poolCreate.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolCreate.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolCreate.queueFamilyIndex = device.GraphicsFamily();
            if (vkCreateCommandPool(m_Device, &poolCreate, nullptr, &pool) != VK_SUCCESS)
                throw std::runtime_error("vkCreateCommandPool for texture upload failed.");
            try
            {
                VkCommandBufferAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate.commandPool = pool;
                allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 1u;
                if (vkAllocateCommandBuffers(m_Device, &allocate, &command) != VK_SUCCESS)
                    throw std::runtime_error("vkAllocateCommandBuffers for texture upload failed.");
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
                    throw std::runtime_error("vkBeginCommandBuffer for texture upload failed.");

                // First real native layout/access transition for every mip:
                // initial undefined bytes -> valid transfer-write destination.
                // This Vulkan command is not generated by RenderGraph.
                VkImageMemoryBarrier toTransfer{};

                toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.image = m_Image;
                toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 1u };
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
                    1u, &toTransfer);

                // Every VkBufferImageCopy maps an authored mip byte offset
                // to the corresponding VkImage mipLevel and extent. Correct
                // ordering depends on the validated texture's Mips sequence.
                std::vector<VkBufferImageCopy> copies;
                copies.reserve(upload.Regions.size());
                for (std::uint32_t level = 0u; level < m_MipLevels; ++level)
                {
                    const auto& region = upload.Regions[level];
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = static_cast<VkDeviceSize>(region.ByteOffset);
                    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u };
                    copy.imageExtent = { region.Width, region.Height, 1u };
                    copies.push_back(copy);
                }
                vkCmdCopyBufferToImage(command, staging.Handle(), m_Image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<std::uint32_t>(copies.size()), copies.data());

                // Publish transfer writes as fragment-shader reads. The
                // descriptor later declares SHADER_READ_ONLY_OPTIMAL and
                // requires this actual per-subresource layout to be true.
                VkImageMemoryBarrier toShader{};

                toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toShader.image = m_Image;
                toShader.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 1u };
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, nullptr, 0u,
                    nullptr, 1u, &toShader);
                if (vkEndCommandBuffer(command) != VK_SUCCESS)
                    throw std::runtime_error("vkEndCommandBuffer for texture upload failed.");
                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.commandBufferCount = 1u;
                submit.pCommandBuffers = &command;
                const auto completion = detail::SubmitAndRetireVulkanUpload(
                    [&] { return vkQueueSubmit(device.GraphicsQueue(), 1u,
                        &submit, VK_NULL_HANDLE); },
                    [&] { return vkQueueWaitIdle(device.GraphicsQueue()); },
                    [&] { return vkDeviceWaitIdle(device.Handle()); },
                    VK_SUCCESS, VK_ERROR_DEVICE_LOST,
                    VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY);

                if (completion.Retirement == detail::VulkanUploadRetirement::Quarantine)
                {
                    // Neither completion nor loss was established. Keep the
                    // native resources alive until the owning VulkanDevice
                    // can safely retire the queue or is itself lost.
                    const auto detached = staging.DetachForDeferredRetirement();
                    device.DeferUncertainTextureUpload({
                        pool, detached.Buffer, detached.Memory, detached.Mapped,
                        m_Image, m_Memory, m_View, m_Sampler
                    });
                    pool = VK_NULL_HANDLE; // journal owns command buffer pool
                    m_Image = VK_NULL_HANDLE;
                    m_Memory = VK_NULL_HANDLE;
                    m_View = VK_NULL_HANDLE;
                    m_Sampler = VK_NULL_HANDLE;
                }
                else if (completion.Retirement == detail::VulkanUploadRetirement::DeviceLost)
                {
                    // Per Vulkan lost-device rules, pending state and
                    // resource-in-use tracking treat DEVICE_LOST as completed.
                    // Ordinary native destruction is therefore permitted, but
                    // the logical device must never receive another upload.
                    device.MarkUnusable();
                }

                if (completion.Retirement != detail::VulkanUploadRetirement::Completed)
                    throw std::runtime_error(
                        "Vulkan texture upload failed (VkResult " +
                        std::to_string(static_cast<int>(completion.Error)) + ").");
            }
            catch (...)
            {
                // On unresolved completion pool is null because ownership
                // moved into the parent device's deferred queue. On ordinary
                // failures or device loss Vulkan allows native cleanup here.
                if (pool != VK_NULL_HANDLE)
                    vkDestroyCommandPool(m_Device, pool, nullptr);
                throw;
            }
            if (pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(m_Device, pool, nullptr);
        }

        // Reverse native ownership: sampler, view, image, then VkDeviceMemory.
        // Caller must retire every descriptor/draw referencing this texture
        // before destroying the underlying VkImage/VkImageView.
        void Destroy() noexcept
        {
            if (m_Sampler != VK_NULL_HANDLE) vkDestroySampler(m_Device, m_Sampler, nullptr);
            if (m_View != VK_NULL_HANDLE) vkDestroyImageView(m_Device, m_View, nullptr);
            if (m_Image != VK_NULL_HANDLE) vkDestroyImage(m_Device, m_Image, nullptr);
            if (m_Memory != VK_NULL_HANDLE) vkFreeMemory(m_Device, m_Memory, nullptr);
            m_Sampler = VK_NULL_HANDLE;
            m_View = VK_NULL_HANDLE;
            m_Image = VK_NULL_HANDLE;
            m_Memory = VK_NULL_HANDLE;
        }
    };
}
