module;

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <utility>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.Runtime;

import Kairo.Renderer.Types;
import Kairo.Renderer.Window;
import Kairo.Renderer.VulkanInstance;
import Kairo.Renderer.VulkanSurface;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.VulkanSwapchain;
import Kairo.Renderer.VulkanCommand;
import Kairo.Renderer.VulkanSync;
import Kairo.Renderer.VulkanTriangle;
import Kairo.Renderer.DebugDraw;
import Kairo.Renderer.VulkanBackendContext;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.RenderScene;
import Kairo.Renderer.ShadowSettings;

export namespace kairo::renderer
{
    /// Signals that the current host can create a GLFW window but cannot expose
    /// a Vulkan presentation surface. This is a capability condition, not a
    /// renderer correctness failure: CI can skip native viewport smoke tests
    /// while retaining all headless/core renderer validation.
    class PresentationUnavailableError final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// Input: native window description.
    /// Output: an owning Vulkan runtime capable of mesh, debug, and tooling
    /// overlay presentation.
    /// Task: keep the window, Vulkan instance, surface, device, swapchain, and
    /// one-frame synchronization objects in destruction-safe order. This is
    /// Later material and render-graph work builds on the same validated
    /// acquire-record-submit-present lifecycle.
    class RendererRuntime final
    {
    public:
        explicit RendererRuntime(const WindowDesc& windowDesc)
            : m_Glfw(), m_Window(windowDesc), m_Instance(MakeInstanceDesc(), RequiredExtensions()), m_Surface(m_Instance, m_Window), m_Device(m_Instance, m_Surface), m_Swapchain(m_Device, m_Surface, m_Window), m_Command(m_Device), m_Sync(m_Device, static_cast<std::uint32_t>(m_Swapchain.Images().size())), m_Triangle(m_Device, m_Swapchain)
        {
        }

        /// Task: render one complete presentation frame. A minimized window
        /// has a zero framebuffer extent and therefore deliberately consumes
        /// no Vulkan work until GLFW restores it. Swapchain invalidation is a
        /// normal resize event, not a fatal renderer error.
        void DrawFrame()
        {
            const auto [width, height] = m_Window.FramebufferExtent();
            if (width == 0u || height == 0u)
            {
                return;
            }

            const VkDevice device = m_Device.Handle();
            const VkFence inFlight = m_Sync.InFlight();
            if (vkWaitForFences(device, 1u, &inFlight, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            {
                throw std::runtime_error("vkWaitForFences failed.");
            }

            std::uint32_t imageIndex = 0;
            const VkResult acquire = vkAcquireNextImageKHR(
                device, m_Swapchain.Handle(), UINT64_MAX, m_Sync.ImageAvailable(), VK_NULL_HANDLE, &imageIndex);
            if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
            {
                RecreateSwapchain();
                return;
            }
            if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
            {
                throw std::runtime_error("vkAcquireNextImageKHR failed.");
            }
            if (vkResetFences(device, 1u, &inFlight) != VK_SUCCESS)
            {
                throw std::runtime_error("vkResetFences failed.");
            }

            RecordFrameCommands(imageIndex);

            // The acquired swapchain image is first accessed as a color
            // attachment by the render pass; waiting at that stage prevents a
            // presentation-to-render hazard on strict Vulkan implementations.
            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            const VkCommandBuffer command = m_Command.Handle();
            const VkSemaphore imageAvailable = m_Sync.ImageAvailable();
            const VkSemaphore renderFinished = m_Sync.RenderFinished(imageIndex);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1u;
            submit.pWaitSemaphores = &imageAvailable;
            submit.pWaitDstStageMask = &waitStage;
            submit.commandBufferCount = 1u;
            submit.pCommandBuffers = &command;
            submit.signalSemaphoreCount = 1u;
            submit.pSignalSemaphores = &renderFinished;
            if (vkQueueSubmit(m_Device.GraphicsQueue(), 1u, &submit, m_Sync.InFlight()) != VK_SUCCESS)
            {
                throw std::runtime_error("vkQueueSubmit failed.");
            }

            const VkSwapchainKHR swapchain = m_Swapchain.Handle();
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1u;
            present.pWaitSemaphores = &renderFinished;
            present.swapchainCount = 1u;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &imageIndex;
            const VkResult result = vkQueuePresentKHR(m_Device.PresentQueue(), &present);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR)
            {
                RecreateSwapchain();
                return;
            }
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error("vkQueuePresentKHR failed.");
            }
        }

        [[nodiscard]] Window& NativeWindow() noexcept { return m_Window; }
        [[nodiscard]] const VulkanInstance& Instance() const noexcept { return m_Instance; }

        /// Input: world-space diagnostic primitives generated by application,
        /// physics, or editor code.
        /// Output: replaces the debug geometry consumed by the next frame.
        /// Task: expose a Vulkan-free submission boundary. The caller owns the
        /// list and may immediately reuse or clear it after this call.
        void SubmitDebugDraw(const DebugDrawList& debug)
        {
            m_Triangle.SetDebugDrawList(debug);
        }

        /// Input: validated CPU indexed geometry.
        /// Output: a stable handle for frame draw extraction.
        /// Task: transfer mesh bytes into renderer-owned Vulkan buffers while
        /// keeping the public handle free of native API types.
        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
            return m_Triangle.CreateMesh(mesh);
        }

        /// Input: a handle returned by CreateMesh.
        /// Task: wait for outstanding GPU use, remove pending draws that
        /// reference the handle, and release its buffers deterministically.
        void DestroyMesh(MeshHandle mesh)
        {
            if (vkDeviceWaitIdle(m_Device.Handle()) != VK_SUCCESS) throw std::runtime_error("vkDeviceWaitIdle failed before mesh destruction.");
            m_Triangle.DestroyMesh(mesh);
        }

        /// Input: renderer-neutral mesh draws for the next and subsequent
        /// frames, until replaced by another submission.
        void SubmitRenderScene(const RenderScene& scene)
        {
            m_Triangle.SetRenderScene(scene);
        }

        /// Input: renderer-neutral directional shadow controls.
        /// Task: expose runtime/editor tuning without publishing Vulkan depth
        /// resources or forcing a swapchain rebuild.
        void SetDirectionalShadowSettings(const DirectionalShadowSettings& settings)
        {
            m_Triangle.SetDirectionalShadowSettings(settings);
        }

        [[nodiscard]] const DirectionalShadowSettings& DirectionalShadows() const noexcept
        {
            return m_Triangle.DirectionalShadows();
        }

        /// Input: renderer-neutral owner supplies a callback that records only
        /// overlay draw commands. Passing an empty callback disables overlays.
        /// Task: integrate tooling UI into the existing scene pass and command
        /// submission lifecycle without exposing begin/end/submit ownership.
        void SetOverlayRecorder(VulkanOverlayRecorder recorder)
        {
            m_OverlayRecorder = std::move(recorder);
        }

        /// Output: current non-owning Vulkan handles for a tooling backend.
        /// Precondition: this RendererRuntime remains alive while handles are used.
        [[nodiscard]] VulkanBackendContext BackendContext() const noexcept
        {
            return {
                m_Instance.Handle(), m_Device.PhysicalHandle(), m_Device.Handle(),
                m_Device.GraphicsQueue(), m_Device.GraphicsFamily(), m_Triangle.RenderPass(),
                static_cast<std::uint32_t>(m_Swapchain.Images().size())
            };
        }
    private:
        GlfwRuntime m_Glfw;
        Window m_Window;
        VulkanInstance m_Instance;
        VulkanSurface m_Surface;
        VulkanDevice m_Device;
        VulkanSwapchain m_Swapchain;
        VulkanCommandBuffer m_Command;
        VulkanFrameSync m_Sync;
        VulkanTriangle m_Triangle;
        VulkanOverlayRecorder m_OverlayRecorder;

        /// Task: record the complete mesh and debug-line render pass.
        void RecordFrameCommands(std::uint32_t imageIndex)
        {
            m_Triangle.Record(m_Command, imageIndex, m_Swapchain.Extent(), m_OverlayRecorder);
        }

        void RecreateSwapchain()
        {
            const auto [width, height] = m_Window.FramebufferExtent();
            if (width == 0u || height == 0u)
            {
                return;
            }
            // Resize can arrive immediately after presentation while the GPU
            // still owns the old depth buffer, framebuffers, and pipelines.
            // M10 has one frame in flight, so an explicit idle point is the
            // simplest correct teardown contract; later render graphs can
            // retire generations with per-frame fences instead.
            if (vkDeviceWaitIdle(m_Device.Handle()) != VK_SUCCESS)
                throw std::runtime_error("vkDeviceWaitIdle failed before swapchain recreation.");
            m_Triangle.ReleaseSwapchainResources();
            m_Swapchain.Recreate(m_Window);
            m_Sync.Recreate(static_cast<std::uint32_t>(m_Swapchain.Images().size()));
            m_Triangle.Recreate(m_Swapchain);
        }

        [[nodiscard]] static VulkanInstanceDesc MakeInstanceDesc() { return { "KairoRenderer", true }; }
        [[nodiscard]] static std::vector<const char*> RequiredExtensions()
        {
            std::uint32_t count = 0;
            const char** names = glfwGetRequiredInstanceExtensions(&count);
            if (names == nullptr || count == 0u)
                throw PresentationUnavailableError("GLFW did not provide Vulkan surface extensions.");
            return { names, names + count };
        }
    };
}
