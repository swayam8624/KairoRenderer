module;

#include <GLFW/glfw3.h>

#include <stdexcept>
#include <vector>

export module Kairo.Renderer.Runtime;

import Kairo.Renderer.Types;
import Kairo.Renderer.Window;
import Kairo.Renderer.VulkanInstance;
import Kairo.Renderer.VulkanSurface;
import Kairo.Renderer.VulkanDevice;

export namespace kairo::renderer
{
    /// Initial renderer-runtime milestone. It deliberately owns only window,
    /// instance, and surface; device/swapchain frames land as the next unit so
    /// initialization failure remains easy to diagnose.
    class RendererRuntime final
    {
    public:
        explicit RendererRuntime(const WindowDesc& windowDesc)
            : m_Glfw(), m_Window(windowDesc), m_Instance(MakeInstanceDesc(), RequiredExtensions()), m_Surface(m_Instance, m_Window), m_Device(m_Instance, m_Surface)
        {
        }
        [[nodiscard]] Window& NativeWindow() noexcept { return m_Window; }
        [[nodiscard]] const VulkanInstance& Instance() const noexcept { return m_Instance; }
    private:
        GlfwRuntime m_Glfw;
        Window m_Window;
        VulkanInstance m_Instance;
        VulkanSurface m_Surface;
        VulkanDevice m_Device;
        [[nodiscard]] static VulkanInstanceDesc MakeInstanceDesc() { return { "KairoRenderer", true }; }
        [[nodiscard]] static std::vector<const char*> RequiredExtensions()
        {
            std::uint32_t count = 0;
            const char** names = glfwGetRequiredInstanceExtensions(&count);
            if (names == nullptr || count == 0u) throw std::runtime_error("GLFW did not provide Vulkan surface extensions.");
            return { names, names + count };
        }
    };
}
