module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>

export module Kairo.Renderer.VulkanShadowMap;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Persistent sampled depth target for one directional light.
    ///
    /// Input: Vulkan device and a bounded non-zero square resolution.
    /// Output: D32 depth image, view, nearest clamp sampler, depth-only render
    /// pass, and framebuffer.
    /// Task: own every resource needed by M10 shadow rendering independently
    /// from swapchain extent. The render pass transitions from discarded depth
    /// contents to shader-read layout each frame and publishes the write to the
    /// following fragment pass through an explicit external dependency.
    class VulkanDirectionalShadowMap final
    {
    public:
        VulkanDirectionalShadowMap(const VulkanDevice& device, std::uint32_t resolution)
            : m_Device(device.Handle()), m_Physical(device.PhysicalHandle()), m_Resolution(resolution)
        {
            if (resolution == 0u || resolution > 8192u)
                throw std::invalid_argument("Directional shadow-map resolution must be in [1, 8192].");
            ValidateFormatSupport();
            try
            {
                CreateImage();
                CreateView();
                CreateSampler();
                CreateRenderPass();
                CreateFramebuffer();
            }
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        ~VulkanDirectionalShadowMap() { Destroy(); }
        VulkanDirectionalShadowMap(const VulkanDirectionalShadowMap&) = delete;
        VulkanDirectionalShadowMap& operator=(const VulkanDirectionalShadowMap&) = delete;

        [[nodiscard]] VkImageView View() const noexcept { return m_View; }
        [[nodiscard]] VkSampler Sampler() const noexcept { return m_Sampler; }
        [[nodiscard]] VkRenderPass RenderPass() const noexcept { return m_RenderPass; }
        [[nodiscard]] VkFramebuffer Framebuffer() const noexcept { return m_Framebuffer; }
        [[nodiscard]] std::uint32_t Resolution() const noexcept { return m_Resolution; }
        [[nodiscard]] static constexpr VkFormat Format() noexcept { return VK_FORMAT_D32_SFLOAT; }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkPhysicalDevice m_Physical = VK_NULL_HANDLE;
        std::uint32_t m_Resolution = 0u;
        VkImage m_Image = VK_NULL_HANDLE;
        VkDeviceMemory m_Memory = VK_NULL_HANDLE;
        VkImageView m_View = VK_NULL_HANDLE;
        VkSampler m_Sampler = VK_NULL_HANDLE;
        VkRenderPass m_RenderPass = VK_NULL_HANDLE;
        VkFramebuffer m_Framebuffer = VK_NULL_HANDLE;

        void ValidateFormatSupport() const
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(m_Physical, Format(), &properties);
            constexpr VkFormatFeatureFlags required =
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error("D32_SFLOAT cannot be both a depth attachment and sampled image on this Vulkan device.");
        }

        void CreateImage()
        {
            VkImageCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            create.imageType = VK_IMAGE_TYPE_2D;
            create.format = Format();
            create.extent = { m_Resolution, m_Resolution, 1u };
            create.mipLevels = 1u;
            create.arrayLayers = 1u;
            create.samples = VK_SAMPLE_COUNT_1_BIT;
            create.tiling = VK_IMAGE_TILING_OPTIMAL;
            create.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_Device, &create, nullptr, &m_Image) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImage for the directional shadow map failed.");

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_Device, m_Image, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = FindDeviceLocalMemory(requirements.memoryTypeBits);
            if (vkAllocateMemory(m_Device, &allocation, nullptr, &m_Memory) != VK_SUCCESS)
                throw std::runtime_error("vkAllocateMemory for the directional shadow map failed.");
            if (vkBindImageMemory(m_Device, m_Image, m_Memory, 0u) != VK_SUCCESS)
                throw std::runtime_error("vkBindImageMemory for the directional shadow map failed.");
        }

        void CreateView()
        {
            VkImageViewCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            create.image = m_Image;
            create.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create.format = Format();
            create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            create.subresourceRange.levelCount = 1u;
            create.subresourceRange.layerCount = 1u;
            if (vkCreateImageView(m_Device, &create, nullptr, &m_View) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImageView for the directional shadow map failed.");
        }

        void CreateSampler()
        {
            VkSamplerCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            create.magFilter = VK_FILTER_NEAREST;
            create.minFilter = VK_FILTER_NEAREST;
            create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            create.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            create.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            create.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            create.maxLod = 0.0f;
            if (vkCreateSampler(m_Device, &create, nullptr, &m_Sampler) != VK_SUCCESS)
                throw std::runtime_error("vkCreateSampler for the directional shadow map failed.");
        }

        void CreateRenderPass()
        {
            VkAttachmentDescription depth{};
            depth.format = Format();
            depth.samples = VK_SAMPLE_COUNT_1_BIT;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            const VkAttachmentReference reference{ 0u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &reference;
            const VkSubpassDependency dependencies[]{
                { VK_SUBPASS_EXTERNAL, 0u,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_DEPENDENCY_BY_REGION_BIT },
                { 0u, VK_SUBPASS_EXTERNAL,
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_DEPENDENCY_BY_REGION_BIT }
            };
            VkRenderPassCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            create.attachmentCount = 1u;
            create.pAttachments = &depth;
            create.subpassCount = 1u;
            create.pSubpasses = &subpass;
            create.dependencyCount = 2u;
            create.pDependencies = dependencies;
            if (vkCreateRenderPass(m_Device, &create, nullptr, &m_RenderPass) != VK_SUCCESS)
                throw std::runtime_error("vkCreateRenderPass for the directional shadow map failed.");
        }

        void CreateFramebuffer()
        {
            VkFramebufferCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            create.renderPass = m_RenderPass;
            create.attachmentCount = 1u;
            create.pAttachments = &m_View;
            create.width = m_Resolution;
            create.height = m_Resolution;
            create.layers = 1u;
            if (vkCreateFramebuffer(m_Device, &create, nullptr, &m_Framebuffer) != VK_SUCCESS)
                throw std::runtime_error("vkCreateFramebuffer for the directional shadow map failed.");
        }

        [[nodiscard]] std::uint32_t FindDeviceLocalMemory(std::uint32_t typeBits) const
        {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(m_Physical, &properties);
            for (std::uint32_t index = 0u; index < properties.memoryTypeCount; ++index)
                if ((typeBits & (1u << index)) != 0u &&
                    (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
                    return index;
            throw std::runtime_error("No device-local memory type supports the directional shadow map.");
        }

        void Destroy() noexcept
        {
            if (m_Framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_Device, m_Framebuffer, nullptr);
            if (m_RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            if (m_Sampler != VK_NULL_HANDLE) vkDestroySampler(m_Device, m_Sampler, nullptr);
            if (m_View != VK_NULL_HANDLE) vkDestroyImageView(m_Device, m_View, nullptr);
            if (m_Image != VK_NULL_HANDLE) vkDestroyImage(m_Device, m_Image, nullptr);
            if (m_Memory != VK_NULL_HANDLE) vkFreeMemory(m_Device, m_Memory, nullptr);
            m_Framebuffer = VK_NULL_HANDLE;
            m_RenderPass = VK_NULL_HANDLE;
            m_Sampler = VK_NULL_HANDLE;
            m_View = VK_NULL_HANDLE;
            m_Image = VK_NULL_HANDLE;
            m_Memory = VK_NULL_HANDLE;
        }
    };
}
