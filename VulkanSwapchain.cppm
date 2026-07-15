module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.VulkanSwapchain;

import Kairo.Renderer.Window;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.VulkanSurface;

export namespace kairo::renderer
{
    /// Input: a present-capable Vulkan device, its native surface, and the
    /// current GLFW framebuffer extent.
    /// Output: owned presentable images and image views.
    /// Task: negotiate surface-dependent render targets. The swapchain must be
    /// recreated after an out-of-date presentation result; callers must not
    /// retain image or image-view handles across Recreate().
    class VulkanSwapchain final
    {
    public:
        VulkanSwapchain(const VulkanDevice& device, const VulkanSurface& surface, const Window& window)
            : m_Device(device.Handle()),
              m_Physical(device.PhysicalHandle()),
              m_Surface(surface.Handle()),
              m_GraphicsFamily(device.GraphicsFamily()),
              m_PresentFamily(device.PresentFamily())
        {
            Create(window);
        }

        ~VulkanSwapchain() { Destroy(); }
        VulkanSwapchain(const VulkanSwapchain&) = delete;
        VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

        [[nodiscard]] VkSwapchainKHR Handle() const noexcept { return m_Handle; }
        [[nodiscard]] VkExtent2D Extent() const noexcept { return m_Extent; }
        [[nodiscard]] VkFormat Format() const noexcept { return m_Format; }
        [[nodiscard]] const std::vector<VkImage>& Images() const noexcept { return m_Images; }
        [[nodiscard]] const std::vector<VkImageView>& ImageViews() const noexcept { return m_ImageViews; }

        /// Precondition: the window framebuffer has non-zero dimensions.
        /// Task: idle the device and rebuild surface-owned images after resize
        /// or presentation invalidation. Idling is correct for one frame in
        /// flight; a future multi-frame scheduler will retire old resources.
        void Recreate(const Window& window)
        {
            if (vkDeviceWaitIdle(m_Device) != VK_SUCCESS)
            {
                throw std::runtime_error("vkDeviceWaitIdle failed during swapchain recreation.");
            }
            Destroy();
            Create(window);
        }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkPhysicalDevice m_Physical = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        std::uint32_t m_GraphicsFamily = 0u;
        std::uint32_t m_PresentFamily = 0u;
        VkSwapchainKHR m_Handle = VK_NULL_HANDLE;
        VkFormat m_Format = VK_FORMAT_B8G8R8A8_SRGB;
        VkExtent2D m_Extent{};
        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;

        void Create(const Window& window)
        {
            VkSurfaceCapabilitiesKHR capabilities{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Physical, m_Surface, &capabilities) != VK_SUCCESS)
            {
                throw std::runtime_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
            }

            const VkSurfaceFormatKHR format = ChooseFormat();
            const VkPresentModeKHR presentMode = ChoosePresentMode();
            m_Extent = ChooseExtent(capabilities, window);
            if (m_Extent.width == 0u || m_Extent.height == 0u)
            {
                throw std::runtime_error("Cannot create a swapchain for a zero-sized framebuffer.");
            }

            std::uint32_t imageCount = capabilities.minImageCount + 1u;
            if (capabilities.maxImageCount > 0u)
            {
                imageCount = std::min(imageCount, capabilities.maxImageCount);
            }

            const std::uint32_t families[]{ m_GraphicsFamily, m_PresentFamily };
            VkSwapchainCreateInfoKHR create{};
            create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            create.surface = m_Surface;
            create.minImageCount = imageCount;
            create.imageFormat = format.format;
            create.imageColorSpace = format.colorSpace;
            create.imageExtent = m_Extent;
            create.imageArrayLayers = 1u;
            create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            create.imageSharingMode = m_GraphicsFamily == m_PresentFamily ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
            create.queueFamilyIndexCount = m_GraphicsFamily == m_PresentFamily ? 0u : 2u;
            create.pQueueFamilyIndices = m_GraphicsFamily == m_PresentFamily ? nullptr : families;
            create.preTransform = capabilities.currentTransform;
            create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            create.presentMode = presentMode;
            create.clipped = VK_TRUE;
            if (vkCreateSwapchainKHR(m_Device, &create, nullptr, &m_Handle) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateSwapchainKHR failed.");
            }

            if (vkGetSwapchainImagesKHR(m_Device, m_Handle, &imageCount, nullptr) != VK_SUCCESS)
            {
                throw std::runtime_error("vkGetSwapchainImagesKHR count query failed.");
            }
            m_Images.resize(imageCount);
            if (vkGetSwapchainImagesKHR(m_Device, m_Handle, &imageCount, m_Images.data()) != VK_SUCCESS)
            {
                throw std::runtime_error("vkGetSwapchainImagesKHR image query failed.");
            }
            m_Format = format.format;

            for (const VkImage image : m_Images)
            {
                VkImageViewCreateInfo view{};
                view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                view.image = image;
                view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view.format = m_Format;
                view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                view.subresourceRange.levelCount = 1u;
                view.subresourceRange.layerCount = 1u;
                VkImageView handle = VK_NULL_HANDLE;
                if (vkCreateImageView(m_Device, &view, nullptr, &handle) != VK_SUCCESS)
                {
                    throw std::runtime_error("vkCreateImageView failed.");
                }
                m_ImageViews.push_back(handle);
            }
        }

        [[nodiscard]] VkSurfaceFormatKHR ChooseFormat() const
        {
            std::uint32_t count = 0u;
            vkGetPhysicalDeviceSurfaceFormatsKHR(m_Physical, m_Surface, &count, nullptr);
            if (count == 0u)
            {
                throw std::runtime_error("The Vulkan surface exposes no image formats.");
            }
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(m_Physical, m_Surface, &count, formats.data());
            for (const auto& format : formats)
            {
                if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                {
                    return format;
                }
            }
            return formats.front();
        }

        [[nodiscard]] VkPresentModeKHR ChoosePresentMode() const
        {
            std::uint32_t count = 0u;
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_Physical, m_Surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_Physical, m_Surface, &count, modes.data());
            return std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()
                ? VK_PRESENT_MODE_MAILBOX_KHR
                : VK_PRESENT_MODE_FIFO_KHR;
        }

        [[nodiscard]] static VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window& window)
        {
            if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
            {
                return capabilities.currentExtent;
            }
            const auto [width, height] = window.FramebufferExtent();
            return {
                std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
                std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)
            };
        }

        void Destroy() noexcept
        {
            for (const VkImageView view : m_ImageViews)
            {
                vkDestroyImageView(m_Device, view, nullptr);
            }
            m_ImageViews.clear();
            m_Images.clear();
            if (m_Handle != VK_NULL_HANDLE)
            {
                vkDestroySwapchainKHR(m_Device, m_Handle, nullptr);
            }
            m_Handle = VK_NULL_HANDLE;
        }
    };
}
