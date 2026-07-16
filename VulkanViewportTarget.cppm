module;

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <stdexcept>

export module Kairo.Renderer.VulkanViewportTarget;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Native texture description borrowed by editor UI backends. The
    /// generation changes whenever Resize() replaces image views, allowing a
    /// consumer to retire and recreate its descriptor without retaining stale
    /// Vulkan handles.
    struct VulkanViewportTexture final
    {
        VkSampler Sampler = VK_NULL_HANDLE;
        VkImageView View = VK_NULL_HANDLE;
        VkImageLayout Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkExtent2D Extent{};
        std::uint64_t Generation = 0u;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return Sampler != VK_NULL_HANDLE && View != VK_NULL_HANDLE &&
                Extent.width > 0u && Extent.height > 0u && Generation > 0u;
        }
    };

    /// Owns the editor scene's offscreen framebuffer.
    ///
    /// Color is HDR and sampled by the editor presentation pass. Object IDs
    /// use an unsigned integer attachment so selection never depends on color
    /// encoding. Depth remains renderer-owned and local to this viewport.
    /// Resize() preserves the compatible render pass and sampler while
    /// replacing only extent-dependent images and the framebuffer.
    class VulkanViewportTarget final
    {
    public:
        VulkanViewportTarget(const VulkanDevice& device, VkExtent2D extent)
            : m_Device(device.Handle()), m_Physical(device.PhysicalHandle())
        {
            ValidateExtent(extent);
            try
            {
                CreateSampler();
                CreateRenderPass();
                CreateAttachments(extent);
                CreateFramebuffer();
            }
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        ~VulkanViewportTarget() { Destroy(); }
        VulkanViewportTarget(const VulkanViewportTarget&) = delete;
        VulkanViewportTarget& operator=(const VulkanViewportTarget&) = delete;

        [[nodiscard]] VkRenderPass RenderPass() const noexcept { return m_RenderPass; }
        [[nodiscard]] VkFramebuffer Framebuffer() const noexcept { return m_Framebuffer; }
        [[nodiscard]] VkExtent2D Extent() const noexcept { return m_Extent; }
        [[nodiscard]] VkImage ObjectIDImage() const noexcept { return m_ObjectID.Image; }
        [[nodiscard]] static constexpr VkFormat ColorFormat() noexcept { return VK_FORMAT_R16G16B16A16_SFLOAT; }
        [[nodiscard]] static constexpr VkFormat ObjectIDFormat() noexcept { return VK_FORMAT_R32_UINT; }
        [[nodiscard]] static constexpr VkFormat DepthFormat() noexcept { return VK_FORMAT_D32_SFLOAT; }

        [[nodiscard]] VulkanViewportTexture Texture() const noexcept
        {
            return { m_Sampler, m_Color.View, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                m_Extent, m_Generation };
        }

        /// Precondition: callers have idled the device and retired commands
        /// using the current framebuffer. A no-op resize preserves generation.
        void Resize(VkExtent2D extent)
        {
            ValidateExtent(extent);
            if (extent.width == m_Extent.width && extent.height == m_Extent.height) return;
            DestroyAttachments();
            try
            {
                CreateAttachments(extent);
                CreateFramebuffer();
            }
            catch (...)
            {
                DestroyAttachments();
                throw;
            }
        }

    private:
        struct Attachment final
        {
            VkImage Image = VK_NULL_HANDLE;
            VkDeviceMemory Memory = VK_NULL_HANDLE;
            VkImageView View = VK_NULL_HANDLE;
        };

        VkDevice m_Device = VK_NULL_HANDLE;
        VkPhysicalDevice m_Physical = VK_NULL_HANDLE;
        VkRenderPass m_RenderPass = VK_NULL_HANDLE;
        VkFramebuffer m_Framebuffer = VK_NULL_HANDLE;
        VkSampler m_Sampler = VK_NULL_HANDLE;
        VkExtent2D m_Extent{};
        Attachment m_Color;
        Attachment m_ObjectID;
        Attachment m_Depth;
        std::uint64_t m_Generation = 0u;

        static void ValidateExtent(VkExtent2D extent)
        {
            if (extent.width == 0u || extent.height == 0u)
                throw std::invalid_argument("VulkanViewportTarget requires a non-zero extent.");
            if (extent.width > 16384u || extent.height > 16384u)
                throw std::invalid_argument("VulkanViewportTarget extent exceeds the 16384 pixel safety limit.");
        }

        void CreateSampler()
        {
            VkSamplerCreateInfo create{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            create.magFilter = VK_FILTER_LINEAR;
            create.minFilter = VK_FILTER_LINEAR;
            create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            create.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            create.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            create.maxLod = 1.0f;
            if (vkCreateSampler(m_Device, &create, nullptr, &m_Sampler) != VK_SUCCESS)
                throw std::runtime_error("vkCreateSampler for editor viewport failed.");
        }

        void CreateRenderPass()
        {
            VkAttachmentDescription color{};
            color.format = ColorFormat();
            color.samples = VK_SAMPLE_COUNT_1_BIT;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkAttachmentDescription objectID{};
            objectID.format = ObjectIDFormat();
            objectID.samples = VK_SAMPLE_COUNT_1_BIT;
            objectID.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            objectID.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            objectID.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            objectID.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

            VkAttachmentDescription depth{};
            depth.format = DepthFormat();
            depth.samples = VK_SAMPLE_COUNT_1_BIT;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            const std::array colorReferences{
                VkAttachmentReference{ 0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
                VkAttachmentReference{ 1u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }
            };
            const VkAttachmentReference depthReference{ 2u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = static_cast<std::uint32_t>(colorReferences.size());
            subpass.pColorAttachments = colorReferences.data();
            subpass.pDepthStencilAttachment = &depthReference;

            std::array<VkSubpassDependency, 2> dependencies{};
            dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[0].dstSubpass = 0u;
            dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
            dependencies[1].srcSubpass = 0u;
            dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT;
            dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

            const std::array attachments{ color, objectID, depth };
            VkRenderPassCreateInfo create{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            create.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            create.pAttachments = attachments.data();
            create.subpassCount = 1u;
            create.pSubpasses = &subpass;
            create.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
            create.pDependencies = dependencies.data();
            if (vkCreateRenderPass(m_Device, &create, nullptr, &m_RenderPass) != VK_SUCCESS)
                throw std::runtime_error("vkCreateRenderPass for editor viewport failed.");
        }

        void CreateAttachments(VkExtent2D extent)
        {
            m_Extent = extent;
            CreateAttachment(m_Color, ColorFormat(), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            CreateAttachment(m_ObjectID, ObjectIDFormat(), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            CreateAttachment(m_Depth, DepthFormat(), VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            ++m_Generation;
            if (m_Generation == 0u) ++m_Generation;
        }

        void CreateAttachment(Attachment& target, VkFormat format,
            VkImageAspectFlags aspect, VkImageUsageFlags usage)
        {
            VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = format;
            image.extent = { m_Extent.width, m_Extent.height, 1u };
            image.mipLevels = 1u;
            image.arrayLayers = 1u;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = usage;
            image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_Device, &image, nullptr, &target.Image) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImage for editor viewport failed.");

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_Device, target.Image, &requirements);
            VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = FindDeviceLocalMemory(requirements.memoryTypeBits);
            if (vkAllocateMemory(m_Device, &allocation, nullptr, &target.Memory) != VK_SUCCESS ||
                vkBindImageMemory(m_Device, target.Image, target.Memory, 0u) != VK_SUCCESS)
                throw std::runtime_error("Cannot allocate editor viewport image memory.");

            VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view.image = target.Image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange.aspectMask = aspect;
            view.subresourceRange.levelCount = 1u;
            view.subresourceRange.layerCount = 1u;
            if (vkCreateImageView(m_Device, &view, nullptr, &target.View) != VK_SUCCESS)
                throw std::runtime_error("vkCreateImageView for editor viewport failed.");
        }

        void CreateFramebuffer()
        {
            const std::array attachments{ m_Color.View, m_ObjectID.View, m_Depth.View };
            VkFramebufferCreateInfo create{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            create.renderPass = m_RenderPass;
            create.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            create.pAttachments = attachments.data();
            create.width = m_Extent.width;
            create.height = m_Extent.height;
            create.layers = 1u;
            if (vkCreateFramebuffer(m_Device, &create, nullptr, &m_Framebuffer) != VK_SUCCESS)
                throw std::runtime_error("vkCreateFramebuffer for editor viewport failed.");
        }

        [[nodiscard]] std::uint32_t FindDeviceLocalMemory(std::uint32_t bits) const
        {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(m_Physical, &properties);
            for (std::uint32_t index = 0u; index < properties.memoryTypeCount; ++index)
                if ((bits & (1u << index)) != 0u &&
                    (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
                    return index;
            throw std::runtime_error("No device-local memory type supports the editor viewport.");
        }

        void DestroyAttachment(Attachment& attachment) noexcept
        {
            if (attachment.View != VK_NULL_HANDLE) vkDestroyImageView(m_Device, attachment.View, nullptr);
            if (attachment.Image != VK_NULL_HANDLE) vkDestroyImage(m_Device, attachment.Image, nullptr);
            if (attachment.Memory != VK_NULL_HANDLE) vkFreeMemory(m_Device, attachment.Memory, nullptr);
            attachment = {};
        }

        void DestroyAttachments() noexcept
        {
            if (m_Framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_Device, m_Framebuffer, nullptr);
            m_Framebuffer = VK_NULL_HANDLE;
            DestroyAttachment(m_Depth);
            DestroyAttachment(m_ObjectID);
            DestroyAttachment(m_Color);
            m_Extent = {};
        }

        void Destroy() noexcept
        {
            DestroyAttachments();
            if (m_RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            if (m_Sampler != VK_NULL_HANDLE) vkDestroySampler(m_Device, m_Sampler, nullptr);
            m_RenderPass = VK_NULL_HANDLE;
            m_Sampler = VK_NULL_HANDLE;
        }
    };
}
