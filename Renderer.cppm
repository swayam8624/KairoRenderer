module;

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <cstdint>
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

export namespace kairo::renderer
{
    /// Input: native window description.
    /// Output: an owning Vulkan runtime capable of presenting a cleared frame.
    /// Task: keep the window, Vulkan instance, surface, device, swapchain, and
    /// one-frame synchronization objects in destruction-safe order. This is
    /// intentionally a minimal rendering baseline: later pipeline work builds
    /// on the same acquire-record-submit-present lifecycle.
    class RendererRuntime final
    {
    public:
        explicit RendererRuntime(const WindowDesc& windowDesc)
            : m_Glfw(), m_Window(windowDesc), m_Instance(MakeInstanceDesc(), RequiredExtensions()), m_Surface(m_Instance, m_Window), m_Device(m_Instance, m_Surface), m_Swapchain(m_Device, m_Surface, m_Window), m_Command(m_Device), m_Sync(m_Device)
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
                m_Swapchain.Recreate(m_Window);
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

            RecordClearCommands(imageIndex);

            const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            const VkCommandBuffer command = m_Command.Handle();
            const VkSemaphore imageAvailable = m_Sync.ImageAvailable();
            const VkSemaphore renderFinished = m_Sync.RenderFinished();
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
                m_Swapchain.Recreate(m_Window);
                return;
            }
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error("vkQueuePresentKHR failed.");
            }
        }

        [[nodiscard]] Window& NativeWindow() noexcept { return m_Window; }
        [[nodiscard]] const VulkanInstance& Instance() const noexcept { return m_Instance; }
    private:
        GlfwRuntime m_Glfw;
        Window m_Window;
        VulkanInstance m_Instance;
        VulkanSurface m_Surface;
        VulkanDevice m_Device;
        VulkanSwapchain m_Swapchain;
        VulkanCommandBuffer m_Command;
        VulkanFrameSync m_Sync;

        /// Task: record a transfer clear. The clear is intentionally pipeline
        /// free, making it a reliable validation point for the frame contract
        /// before shaders, render passes, and mesh resources are introduced.
        void RecordClearCommands(std::uint32_t imageIndex)
        {
            m_Command.Begin();
            const VkImage image = m_Swapchain.Images().at(imageIndex);
            VkImageMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcAccessMask = 0u;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.image = image;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.levelCount = 1u;
            toTransfer.subresourceRange.layerCount = 1u;
            vkCmdPipelineBarrier(
                m_Command.Handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
                0u, nullptr, 0u, nullptr, 1u, &toTransfer);

            const VkClearColorValue clear{ { 0.035f, 0.095f, 0.18f, 1.0f } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
            vkCmdClearColorImage(m_Command.Handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1u, &range);

            VkImageMemoryBarrier toPresent{};
            toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toPresent.dstAccessMask = 0u;
            toPresent.image = image;
            toPresent.subresourceRange = range;
            vkCmdPipelineBarrier(
                m_Command.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0u,
                0u, nullptr, 0u, nullptr, 1u, &toPresent);
            m_Command.End();
        }

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
