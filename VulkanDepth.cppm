module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>

// TEACHING DEVICE-LOCAL DEPTH: reusable one-image D32 attachment for simple
// framebuffer paths. Unlike VulkanShadowMap, this image has no SAMPLED usage;
// unlike VulkanViewportTarget, it does not own an HDR/ID attachment pair.
// The render pass sets the actual native depth layout.
export module Kairo.Renderer.VulkanDepth;

import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Input: a physical/logical device pair and a non-zero framebuffer extent.
    /// Output: one device-local D32 depth image and matching image view.
    /// Task: provide a swapchain-sized depth attachment for raster pipelines.
    /// It owns no layout transition itself; the render pass establishes the
    /// first depth layout and later passes declare their own dependencies.
    // Own VkImage, VkDeviceMemory and a depth-aspect VkImageView; borrow the
    // logical/physical Vulkan device. Framebuffer/pipeline attachment users
    // must retire and release references before this object is destroyed.
    class VulkanDepthAttachment final
    {
    public:
        // Reject zero dimensions, then allocate a native depth image.
        // Constructor failure explicitly invokes Destroy() to release any
        // image, memory or view successfully created before a later throw.
        // The normal C++ destructor does not run for an incomplete object.
        VulkanDepthAttachment(const VulkanDevice& device, VkExtent2D extent)
            : m_Device(device.Handle()), m_Physical(device.PhysicalHandle())
        {
            if (extent.width == 0u || extent.height == 0u)
            {
                throw std::invalid_argument("VulkanDepthAttachment requires a non-zero extent.");
            }
            try { Create(device.PhysicalHandle(), extent); }
            catch (...) { Destroy(); throw; }
        }

        ~VulkanDepthAttachment() { Destroy(); }

        VulkanDepthAttachment(const VulkanDepthAttachment&) = delete;
        VulkanDepthAttachment& operator=(const VulkanDepthAttachment&) = delete;

        [[nodiscard]] VkImageView View() const noexcept { return m_View; }
        [[nodiscard]] VkFormat Format() const noexcept { return VK_FORMAT_D32_SFLOAT; }

        /// Precondition: the device is idle and no framebuffer retains View().
        /// Task: rebuild the depth image to match a recreated swapchain extent.
        // Requires device idle and release of every framebuffer using View().
        // Destroys old image before building new; failure is NOT transactional.
        void Recreate(VkExtent2D extent)
        {
            if (extent.width == 0u || extent.height == 0u)
            {
                throw std::invalid_argument("VulkanDepthAttachment requires a non-zero extent.");
            }
            Destroy();
            // Preserve the current destroy-before-replace resize policy, but
            // leave no partial native allocations if the replacement fails.
            try { Create(m_Physical, extent); }
            catch (...) { Destroy(); throw; }
        }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkPhysicalDevice m_Physical = VK_NULL_HANDLE;
        VkImage m_Image = VK_NULL_HANDLE;
        VkDeviceMemory m_Memory = VK_NULL_HANDLE;
        VkImageView m_View = VK_NULL_HANDLE;

        // Release view first, then image, then bound device-local memory;
        // clearing handles permits internal cleanup on a partial build.
        void Destroy() noexcept
        {
            if (m_View != VK_NULL_HANDLE) vkDestroyImageView(m_Device, m_View, nullptr);
            if (m_Image != VK_NULL_HANDLE) vkDestroyImage(m_Device, m_Image, nullptr);
            if (m_Memory != VK_NULL_HANDLE) vkFreeMemory(m_Device, m_Memory, nullptr);
            m_View = VK_NULL_HANDLE;
            m_Image = VK_NULL_HANDLE;
            m_Memory = VK_NULL_HANDLE;
        }

        // Create optimal-tiled one-mip D32 image with DEPTH_ATTACHMENT usage,
        // allocate compatible device-local memory, bind, and expose depth
        // aspect only through VkImageView. The render pass (not this method)
        // establishes the actual depth-write image layout on first use.
        void Create(VkPhysicalDevice physical, VkExtent2D extent)
        {
            VkImageCreateInfo image{};
            image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image.imageType = VK_IMAGE_TYPE_2D;
            image.format = Format();
            image.extent = { extent.width, extent.height, 1u };
            image.mipLevels = 1u;
            image.arrayLayers = 1u;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_Device, &image, nullptr, &m_Image) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateImage for depth attachment failed.");
            }
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_Device, m_Image, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = FindDeviceLocalMemory(physical, requirements.memoryTypeBits);
            if (vkAllocateMemory(m_Device, &allocation, nullptr, &m_Memory) != VK_SUCCESS ||
                vkBindImageMemory(m_Device, m_Image, m_Memory, 0u) != VK_SUCCESS)
            {
                throw std::runtime_error("Failed to allocate or bind Vulkan depth memory.");
            }
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = m_Image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = Format();
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view.subresourceRange.levelCount = 1u;
            view.subresourceRange.layerCount = 1u;
            if (vkCreateImageView(m_Device, &view, nullptr, &m_View) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateImageView for depth attachment failed.");
            }
        }

        // Intersect the image's requirements mask with DEVICE_LOCAL memory
        // properties, failing explicitly when this allocation is unsupported.
        [[nodiscard]] static std::uint32_t FindDeviceLocalMemory(VkPhysicalDevice physical, std::uint32_t bits)
        {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physical, &properties);
            for (std::uint32_t index = 0u; index < properties.memoryTypeCount; ++index)
            {
                if ((bits & (1u << index)) != 0u &&
                    (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
                {
                    return index;
                }
            }
            throw std::runtime_error("No device-local Vulkan memory type supports the depth image.");
        }
    };
}
