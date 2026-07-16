module;

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

export module Kairo.Renderer.Runtime;

import Kairo.Renderer.Types;
import Kairo.Renderer.Camera;
import Kairo.Renderer.Window;
import Kairo.Renderer.VulkanInstance;
import Kairo.Renderer.VulkanSurface;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.VulkanSwapchain;
import Kairo.Renderer.VulkanCommand;
import Kairo.Renderer.VulkanSync;
import Kairo.Renderer.VulkanTriangle;
import Kairo.Renderer.VulkanViewportTarget;
import Kairo.Renderer.VulkanBuffer;
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

    struct ViewportCapture final
    {
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::vector<std::uint8_t> RGBA;

        [[nodiscard]] bool IsVisuallyNonUniform(std::uint8_t minimumRange = 4u) const noexcept
        {
            if (RGBA.size() != static_cast<std::size_t>(Width) * Height * 4u || RGBA.empty()) return false;
            std::uint8_t minimum = 255u;
            std::uint8_t maximum = 0u;
            for (std::size_t index = 0u; index < RGBA.size(); index += 4u)
                for (std::size_t channel = 0u; channel < 3u; ++channel)
                {
                    minimum = std::min(minimum, RGBA[index + channel]);
                    maximum = std::max(maximum, RGBA[index + channel]);
                }
            return static_cast<unsigned>(maximum) - minimum >= minimumRange;
        }
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
            : m_Glfw(), m_Window(windowDesc), m_Instance(MakeInstanceDesc(), RequiredExtensions()), m_Surface(m_Instance, m_Window), m_Device(m_Instance, m_Surface), m_Swapchain(m_Device, m_Surface, m_Window), m_Command(m_Device), m_Sync(m_Device, static_cast<std::uint32_t>(m_Swapchain.Images().size())), m_Triangle(m_Device, m_Swapchain), m_PickReadback(m_Device, sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT)
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
            CompleteViewportPick();
            CompleteViewportCapture();

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
            if (m_PickRequested.has_value())
            {
                m_PickInFlight = true;
                m_PickRequested.reset();
            }
            if (m_CaptureRequested)
            {
                m_CaptureRequested = false;
                m_CaptureInFlight = true;
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

        /// Input: a finite right-handed camera pose supplied by the host.
        /// Output: the next frame uses the corresponding view matrix.
        /// Task: provide a narrow editor/game camera boundary without exposing
        /// Vulkan camera buffers or requiring the renderer to process input.
        void SetCameraPose(const CameraPose& pose)
        {
            m_Triangle.SetCameraPose(pose);
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

        /// Input: one diagnostic shading policy.
        /// Task: switch the sampled editor viewport without rebuilding Vulkan
        /// pipelines or changing the submitted scene.
        void SetViewportShadingMode(ViewportShadingMode mode) noexcept
        {
            m_Triangle.SetViewportShadingMode(mode);
        }

        [[nodiscard]] ViewportShadingMode ViewportShading() const noexcept
        {
            return m_Triangle.ViewportShading();
        }

        /// Output: borrowed sampled scene texture for native editor backends.
        /// The generation changes after ResizeViewport replaces image views.
        [[nodiscard]] VulkanViewportTexture ViewportTexture() const noexcept
        {
            return m_Triangle.ViewportTexture();
        }

        /// Input: physical-pixel dimensions requested by the editor panel.
        /// Task: resize only the offscreen scene target. One frame in flight
        /// makes a device idle point simple and correct for this boundary.
        void ResizeViewport(std::uint32_t width, std::uint32_t height)
        {
            const auto current = m_Triangle.ViewportTexture().Extent;
            if (current.width == width && current.height == height) return;
            if (width == 0u || height == 0u) return;
            if (vkDeviceWaitIdle(m_Device.Handle()) != VK_SUCCESS)
                throw std::runtime_error("vkDeviceWaitIdle failed before editor viewport resize.");
            m_Triangle.ResizeViewport({ width, height });
        }

        /// Input: physical pixel coordinates local to the sampled viewport.
        /// Task: queue a non-blocking object-ID readback for the next frame.
        /// Repeated requests before submission retain only the newest click.
        void RequestViewportPick(std::uint32_t x, std::uint32_t y)
        {
            const VkExtent2D extent = m_Triangle.ViewportTexture().Extent;
            if (x >= extent.width || y >= extent.height)
                throw std::out_of_range("Viewport pick lies outside the render target.");
            m_PickRequested = VkOffset2D{ static_cast<std::int32_t>(x), static_cast<std::int32_t>(y) };
        }

        /// Output: completed stable entity ID, where zero means background.
        [[nodiscard]] std::optional<std::uint32_t> TakeViewportPickResult() noexcept
        {
            return std::exchange(m_PickResult, std::nullopt);
        }

        /// Task: queue one complete HDR viewport readback without stalling the
        /// current frame. Completion is available after the next fence wait.
        void RequestViewportCapture()
        {
            if (m_CaptureRequested || m_CaptureInFlight)
                throw std::logic_error("A viewport capture is already pending.");
            const VkExtent2D extent = m_Triangle.ViewportTexture().Extent;
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 8u;
            m_CaptureReadback = std::make_unique<VulkanHostBuffer>(
                m_Device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            m_CaptureExtent = extent;
            m_CaptureRequested = true;
        }

        [[nodiscard]] std::optional<ViewportCapture> TakeViewportCapture() noexcept
        {
            return std::exchange(m_CaptureResult, std::nullopt);
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
        VulkanHostBuffer m_PickReadback;
        VulkanOverlayRecorder m_OverlayRecorder;
        std::optional<VkOffset2D> m_PickRequested;
        std::optional<std::uint32_t> m_PickResult;
        bool m_PickInFlight = false;
        std::unique_ptr<VulkanHostBuffer> m_CaptureReadback;
        VkExtent2D m_CaptureExtent{};
        std::optional<ViewportCapture> m_CaptureResult;
        bool m_CaptureRequested = false;
        bool m_CaptureInFlight = false;

        /// Task: record the complete mesh and debug-line render pass.
        void RecordFrameCommands(std::uint32_t imageIndex)
        {
            m_Triangle.Record(m_Command, imageIndex, m_Swapchain.Extent(), m_OverlayRecorder,
                m_PickRequested.has_value() ? m_PickReadback.Handle() : VK_NULL_HANDLE,
                m_PickRequested,
                m_CaptureRequested ? m_CaptureReadback->Handle() : VK_NULL_HANDLE);
        }

        void CompleteViewportPick()
        {
            if (!m_PickInFlight) return;
            std::uint32_t id = 0u;
            m_PickReadback.Read(&id, sizeof(id));
            m_PickResult = id;
            m_PickInFlight = false;
        }

        [[nodiscard]] static float HalfToFloat(std::uint16_t half) noexcept
        {
            const bool negative = (half & 0x8000u) != 0u;
            const std::uint16_t exponent = (half >> 10u) & 0x1fu;
            const std::uint16_t mantissa = half & 0x03ffu;
            float value = 0.0f;
            if (exponent == 0u) value = std::ldexp(static_cast<float>(mantissa), -24);
            else if (exponent == 31u) value = mantissa == 0u
                ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
            else value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                static_cast<int>(exponent) - 15);
            return negative ? -value : value;
        }

        void CompleteViewportCapture()
        {
            if (!m_CaptureInFlight) return;
            const std::size_t pixelCount = static_cast<std::size_t>(m_CaptureExtent.width) * m_CaptureExtent.height;
            std::vector<std::uint16_t> source(pixelCount * 4u);
            m_CaptureReadback->Read(source.data(), source.size() * sizeof(std::uint16_t));
            ViewportCapture capture{ m_CaptureExtent.width, m_CaptureExtent.height,
                std::vector<std::uint8_t>(pixelCount * 4u) };
            for (std::size_t index = 0u; index < pixelCount; ++index)
            {
                for (std::size_t channel = 0u; channel < 3u; ++channel)
                {
                    float linear = HalfToFloat(source[index * 4u + channel]);
                    if (!std::isfinite(linear)) linear = 0.0f;
                    linear = std::clamp(linear, 0.0f, 1.0f);
                    capture.RGBA[index * 4u + channel] = static_cast<std::uint8_t>(
                        std::lround(std::pow(linear, 1.0f / 2.2f) * 255.0f));
                }
                capture.RGBA[index * 4u + 3u] = 255u;
            }
            m_CaptureResult = std::move(capture);
            m_CaptureReadback.reset();
            m_CaptureInFlight = false;
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
