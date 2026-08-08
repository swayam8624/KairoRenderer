module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

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
    class VulkanTexture2D final
    {
    public:
        VulkanTexture2D(const VulkanDevice& device,
            const kairo::assets::TextureArtifactData& texture,
            TextureSampler sampling = {})
            : m_Device(device.Handle())
        {
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

        void CreateImage(const VulkanDevice& device)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device.PhysicalHandle(), m_Format, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0u)
                throw std::runtime_error("Selected Vulkan device cannot sample the texture artifact format.");

            VkImageCreateInfo create{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
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
            VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = FindMemoryType(device.PhysicalHandle(),
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(m_Device, &allocation, nullptr, &m_Memory) != VK_SUCCESS)
                throw std::runtime_error("vkAllocateMemory for texture failed.");
            if (vkBindImageMemory(m_Device, m_Image, m_Memory, 0u) != VK_SUCCESS)
                throw std::runtime_error("vkBindImageMemory for texture failed.");
        }

        void CreateView()
        {
            VkImageViewCreateInfo create{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            create.image = m_Image;
            create.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create.format = m_Format;
            create.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 1u };
            if (vkCreateImageView(m_Device, &create, nullptr, &m_View) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImageView for texture failed.");
        }

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

        void CreateSampler(TextureSampler sampling)
        {
            VkSamplerCreateInfo create{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
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

        void Upload(const VulkanDevice& device,
            const kairo::assets::TextureArtifactData& texture)
        {
            std::size_t totalBytes = 0u;
            for (const auto& mip : texture.Mips) totalBytes += mip.Pixels.size();
            VulkanHostBuffer staging(device, static_cast<VkDeviceSize>(totalBytes),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            std::vector<std::byte> packed;
            packed.reserve(totalBytes);
            for (const auto& mip : texture.Mips)
                packed.insert(packed.end(), mip.Pixels.begin(), mip.Pixels.end());
            staging.Write(packed.data(), static_cast<VkDeviceSize>(packed.size()));

            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer command = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo poolCreate{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolCreate.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolCreate.queueFamilyIndex = device.GraphicsFamily();
            if (vkCreateCommandPool(m_Device, &poolCreate, nullptr, &pool) != VK_SUCCESS)
                throw std::runtime_error("vkCreateCommandPool for texture upload failed.");
            try
            {
                VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocate.commandPool = pool;
                allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 1u;
                if (vkAllocateCommandBuffers(m_Device, &allocate, &command) != VK_SUCCESS)
                    throw std::runtime_error("vkAllocateCommandBuffers for texture upload failed.");
                VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
                    throw std::runtime_error("vkBeginCommandBuffer for texture upload failed.");

                VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
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

                std::vector<VkBufferImageCopy> copies;
                copies.reserve(texture.Mips.size());
                VkDeviceSize offset = 0u;
                for (std::uint32_t level = 0u; level < m_MipLevels; ++level)
                {
                    const auto& mip = texture.Mips[level];
                    VkBufferImageCopy copy{};
                    copy.bufferOffset = offset;
                    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u };
                    copy.imageExtent = { mip.Width, mip.Height, 1u };
                    copies.push_back(copy);
                    offset += static_cast<VkDeviceSize>(mip.Pixels.size());
                }
                vkCmdCopyBufferToImage(command, staging.Handle(), m_Image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<std::uint32_t>(copies.size()), copies.data());

                VkImageMemoryBarrier toShader{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
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
                VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                submit.commandBufferCount = 1u;
                submit.pCommandBuffers = &command;
                if (vkQueueSubmit(device.GraphicsQueue(), 1u, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
                    vkQueueWaitIdle(device.GraphicsQueue()) != VK_SUCCESS)
                    throw std::runtime_error("Vulkan texture upload submission failed.");
            }
            catch (...)
            {
                vkDestroyCommandPool(m_Device, pool, nullptr);
                throw;
            }
            vkDestroyCommandPool(m_Device, pool, nullptr);
        }

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
