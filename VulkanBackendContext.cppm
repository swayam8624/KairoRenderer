module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>

export module Kairo.Renderer.VulkanBackendContext;

export namespace kairo::renderer
{
    /// Non-owning renderer integration snapshot for Vulkan-based tooling.
    /// Input: obtained from RendererRuntime::BackendContext while that runtime
    /// remains alive. Output: handles required by an overlay backend such as
    /// Dear ImGui. Task: expose integration capability without transferring
    /// resource ownership or making tools reach into renderer internals.
    ///
    /// The render pass is recreated with the swapchain. Consumers must query a
    /// fresh snapshot after a renderer resize notification before rebuilding
    /// resources that are not render-pass compatible.
    struct VulkanBackendContext final
    {
        VkInstance Instance = VK_NULL_HANDLE;
        VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
        VkDevice Device = VK_NULL_HANDLE;
        VkQueue GraphicsQueue = VK_NULL_HANDLE;
        std::uint32_t GraphicsQueueFamily = 0u;
        VkRenderPass RenderPass = VK_NULL_HANDLE;
        std::uint32_t SwapchainImageCount = 0u;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return Instance != VK_NULL_HANDLE && PhysicalDevice != VK_NULL_HANDLE && Device != VK_NULL_HANDLE &&
                GraphicsQueue != VK_NULL_HANDLE && RenderPass != VK_NULL_HANDLE && SwapchainImageCount > 0u;
        }
    };

    /// Called while the renderer's graphics render pass is active. The
    /// callback records overlay commands only; it must not begin/end the render
    /// pass, submit the command buffer, or destroy renderer-owned resources.
    using VulkanOverlayRecorder = std::function<void(VkCommandBuffer)>;
}
