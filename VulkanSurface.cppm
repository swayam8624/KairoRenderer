module;

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include <stdexcept>

export module Kairo.Renderer.VulkanSurface;

import Kairo.Renderer.Window;
import Kairo.Renderer.VulkanInstance;

export namespace kairo::renderer
{
    /// Input: live VulkanInstance and GLFW Window.
    /// Output: platform surface destroyed before its instance.
    /// Task: make surface ownership explicit so a later device/swapchain layer
    /// never relies on borrowed native-window lifetime.
    class VulkanSurface final
    {
    public:
        VulkanSurface(const VulkanInstance& instance, const Window& window)
            : m_Instance(instance.Handle())
        {
            if (glfwCreateWindowSurface(m_Instance, window.NativeHandle(), nullptr, &m_Handle) != VK_SUCCESS)
            {
                throw std::runtime_error("glfwCreateWindowSurface failed.");
            }
        }
        ~VulkanSurface() { if (m_Handle != VK_NULL_HANDLE) vkDestroySurfaceKHR(m_Instance, m_Handle, nullptr); }
        VulkanSurface(const VulkanSurface&) = delete;
        VulkanSurface& operator=(const VulkanSurface&) = delete;
        [[nodiscard]] VkSurfaceKHR Handle() const noexcept { return m_Handle; }
    private:
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkSurfaceKHR m_Handle = VK_NULL_HANDLE;
    };
}
