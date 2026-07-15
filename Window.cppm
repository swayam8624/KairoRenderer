module;

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <stdexcept>
#include <utility>

export module Kairo.Renderer.Window;

import Kairo.Renderer.Types;

export namespace kairo::renderer
{
    /// RAII owner for the GLFW process lifetime. One instance must outlive all
    /// windows so GLFW termination cannot invalidate a native Vulkan surface.
    class GlfwRuntime final
    {
    public:
        GlfwRuntime()
        {
#if defined(__APPLE__) && defined(KAIRO_RENDERER_MOLTENVK_ICD_FILE)
            // Homebrew installations can coexist with a legacy MoltenVK ICD
            // under /usr/local. Prefer this build's configured ICD, but honor
            // a caller's explicit VK_ICD_FILENAMES override for diagnostics.
            const char* configuredIcd = std::getenv("VK_ICD_FILENAMES");
            if (configuredIcd == nullptr || configuredIcd[0] == '\0')
            {
                if (setenv("VK_ICD_FILENAMES", KAIRO_RENDERER_MOLTENVK_ICD_FILE, 0) != 0)
                {
                    throw std::runtime_error("Failed to configure the MoltenVK ICD path.");
                }
            }
#endif
            if (glfwInit() != GLFW_TRUE)
            {
                throw std::runtime_error("GLFW initialization failed.");
            }
        }
        ~GlfwRuntime() { glfwTerminate(); }
        GlfwRuntime(const GlfwRuntime&) = delete;
        GlfwRuntime& operator=(const GlfwRuntime&) = delete;
    };

    /// Input: WindowDesc plus a live GlfwRuntime.
    /// Output: a GLFW no-client-API window suitable for Vulkan surface creation.
    /// Task: own native lifetime, events, resize state, and framebuffer extent
    /// without leaking GLFW into renderer clients.
    class Window final
    {
    public:
        explicit Window(const WindowDesc& desc)
        {
            ValidateWindowDesc(desc);
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, desc.Resizable ? GLFW_TRUE : GLFW_FALSE);
            m_Handle = glfwCreateWindow(
                static_cast<int>(desc.Width), static_cast<int>(desc.Height), desc.Title.c_str(), nullptr, nullptr);
            if (m_Handle == nullptr)
            {
                throw std::runtime_error("GLFW window creation failed.");
            }
        }
        ~Window() { if (m_Handle != nullptr) glfwDestroyWindow(m_Handle); }
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        [[nodiscard]] GLFWwindow* NativeHandle() const noexcept { return m_Handle; }
        [[nodiscard]] bool ShouldClose() const noexcept { return glfwWindowShouldClose(m_Handle) == GLFW_TRUE; }
        void PollEvents() const noexcept { glfwPollEvents(); }
        void RequestClose() const noexcept { glfwSetWindowShouldClose(m_Handle, GLFW_TRUE); }
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> FramebufferExtent() const noexcept
        {
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(m_Handle, &width, &height);
            return { static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
        }
    private:
        GLFWwindow* m_Handle = nullptr;
    };
}
