module;

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

export module Kairo.Renderer.VulkanTriangle;

import Kairo.Renderer.VulkanCommand;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.VulkanSwapchain;

export namespace kairo::renderer
{
    /// Input: device and the currently valid swapchain images.
    /// Output: a render pass, per-image framebuffers, and a graphics pipeline.
    /// Task: establish the renderer's first complete raster path. The vertex
    /// shader uses gl_VertexIndex only; vertex buffers and mesh resources are
    /// deliberately deferred to the next milestone.
    class VulkanTriangle final
    {
    public:
        VulkanTriangle(const VulkanDevice& device, const VulkanSwapchain& swapchain)
            : m_Device(device.Handle())
        {
            Create(swapchain);
        }

        ~VulkanTriangle() { Destroy(); }
        VulkanTriangle(const VulkanTriangle&) = delete;
        VulkanTriangle& operator=(const VulkanTriangle&) = delete;

        /// Task: destroy swapchain-dependent framebuffers and rebuild the
        /// render pass/pipeline after a resize or surface format change.
        void Recreate(const VulkanSwapchain& swapchain)
        {
            Destroy();
            Create(swapchain);
        }

        /// Task: release objects that reference swapchain image views before
        /// the swapchain itself destroys those views during recreation.
        void ReleaseSwapchainResources() noexcept { Destroy(); }

        /// Precondition: imageIndex was acquired from the matching swapchain.
        /// Task: encode clear, viewport/scissor setup, and one triangle draw.
        void Record(VulkanCommandBuffer& command, std::uint32_t imageIndex, VkExtent2D extent) const
        {
            command.Begin();
            const VkClearValue clear{ { { 0.025f, 0.055f, 0.11f, 1.0f } } };
            VkRenderPassBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = m_RenderPass;
            begin.framebuffer = m_Framebuffers.at(imageIndex);
            begin.renderArea.extent = extent;
            begin.clearValueCount = 1u;
            begin.pClearValues = &clear;
            vkCmdBeginRenderPass(command.Handle(), &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.extent = extent;
            vkCmdSetViewport(command.Handle(), 0u, 1u, &viewport);
            vkCmdSetScissor(command.Handle(), 0u, 1u, &scissor);
            vkCmdDraw(command.Handle(), 3u, 1u, 0u, 0u);
            vkCmdEndRenderPass(command.Handle());
            command.End();
        }

    private:
        VkDevice m_Device = VK_NULL_HANDLE;
        VkRenderPass m_RenderPass = VK_NULL_HANDLE;
        VkPipelineLayout m_Layout = VK_NULL_HANDLE;
        VkPipeline m_Pipeline = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> m_Framebuffers;

        void Create(const VulkanSwapchain& swapchain)
        {
            try
            {
                CreateRenderPass(swapchain.Format());
                CreateFramebuffers(swapchain);
                CreatePipeline();
            }
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        void CreateRenderPass(VkFormat format)
        {
            VkAttachmentDescription color{};
            color.format = format;
            color.samples = VK_SAMPLE_COUNT_1_BIT;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference reference{};
            reference.attachment = 0u;
            reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1u;
            subpass.pColorAttachments = &reference;
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0u;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            create.attachmentCount = 1u;
            create.pAttachments = &color;
            create.subpassCount = 1u;
            create.pSubpasses = &subpass;
            create.dependencyCount = 1u;
            create.pDependencies = &dependency;
            if (vkCreateRenderPass(m_Device, &create, nullptr, &m_RenderPass) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateRenderPass failed.");
            }
        }

        void CreateFramebuffers(const VulkanSwapchain& swapchain)
        {
            for (const VkImageView imageView : swapchain.ImageViews())
            {
                VkFramebufferCreateInfo create{};
                create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                create.renderPass = m_RenderPass;
                create.attachmentCount = 1u;
                create.pAttachments = &imageView;
                create.width = swapchain.Extent().width;
                create.height = swapchain.Extent().height;
                create.layers = 1u;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(m_Device, &create, nullptr, &framebuffer) != VK_SUCCESS)
                {
                    throw std::runtime_error("vkCreateFramebuffer failed.");
                }
                m_Framebuffers.push_back(framebuffer);
            }
        }

        void CreatePipeline()
        {
            const VkShaderModule vertex = CreateShaderModule(ReadSpirv("triangle.vert.spv"));
            const VkShaderModule fragment = CreateShaderModule(ReadSpirv("triangle.frag.spv"));
            try
            {
                CreatePipeline(vertex, fragment);
            }
            catch (...)
            {
                vkDestroyShaderModule(m_Device, fragment, nullptr);
                vkDestroyShaderModule(m_Device, vertex, nullptr);
                throw;
            }
            vkDestroyShaderModule(m_Device, fragment, nullptr);
            vkDestroyShaderModule(m_Device, vertex, nullptr);
        }

        void CreatePipeline(VkShaderModule vertex, VkShaderModule fragment)
        {
            VkPipelineShaderStageCreateInfo vertexStage{};
            vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vertexStage.module = vertex;
            vertexStage.pName = "main";
            VkPipelineShaderStageCreateInfo fragmentStage{};
            fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fragmentStage.module = fragment;
            fragmentStage.pName = "main";
            const std::array stages{ vertexStage, fragmentStage };

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            VkPipelineInputAssemblyStateCreateInfo assembly{};
            assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{};
            viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport.viewportCount = 1u;
            viewport.scissorCount = 1u;
            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blendAttachment{};
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{};
            blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            blend.attachmentCount = 1u;
            blend.pAttachments = &blendAttachment;
            const std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
            dynamic.pDynamicStates = dynamicStates.data();
            VkPipelineLayoutCreateInfo layout{};
            layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            if (vkCreatePipelineLayout(m_Device, &layout, nullptr, &m_Layout) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreatePipelineLayout failed.");
            }
            VkGraphicsPipelineCreateInfo pipeline{};
            pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipeline.stageCount = static_cast<std::uint32_t>(stages.size());
            pipeline.pStages = stages.data();
            pipeline.pVertexInputState = &vertexInput;
            pipeline.pInputAssemblyState = &assembly;
            pipeline.pViewportState = &viewport;
            pipeline.pRasterizationState = &rasterizer;
            pipeline.pMultisampleState = &multisampling;
            pipeline.pColorBlendState = &blend;
            pipeline.pDynamicState = &dynamic;
            pipeline.layout = m_Layout;
            pipeline.renderPass = m_RenderPass;
            if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1u, &pipeline, nullptr, &m_Pipeline) != VK_SUCCESS)
            {
                vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
                m_Layout = VK_NULL_HANDLE;
                throw std::runtime_error("vkCreateGraphicsPipelines failed.");
            }
        }

        [[nodiscard]] VkShaderModule CreateShaderModule(const std::vector<std::uint32_t>& spirv) const
        {
            VkShaderModuleCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            create.codeSize = spirv.size() * sizeof(std::uint32_t);
            create.pCode = spirv.data();
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(m_Device, &create, nullptr, &module) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateShaderModule failed.");
            }
            return module;
        }

        [[nodiscard]] static std::vector<std::uint32_t> ReadSpirv(const std::string& fileName)
        {
            const std::filesystem::path path = std::filesystem::path(KAIRO_RENDERER_SHADER_DIR) / fileName;
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
            {
                throw std::runtime_error("Cannot open shader binary: " + path.string());
            }
            const std::streamsize bytes = input.tellg();
            if (bytes <= 0 || bytes % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0)
            {
                throw std::runtime_error("Shader binary is empty or not aligned to 32-bit SPIR-V words: " + path.string());
            }
            std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char*>(words.data()), bytes))
            {
                throw std::runtime_error("Failed to read shader binary: " + path.string());
            }
            return words;
        }

        void Destroy() noexcept
        {
            for (const VkFramebuffer framebuffer : m_Framebuffers)
            {
                vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
            }
            m_Framebuffers.clear();
            if (m_Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
            if (m_Layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
            if (m_RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            m_Pipeline = VK_NULL_HANDLE;
            m_Layout = VK_NULL_HANDLE;
            m_RenderPass = VK_NULL_HANDLE;
        }
    };
}
