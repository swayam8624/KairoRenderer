module;

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

export module Kairo.Renderer.VulkanTriangle;

import Kairo.Renderer.Camera;
import Kairo.Renderer.DebugDraw;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.VulkanBuffer;
import Kairo.Renderer.VulkanCommand;
import Kairo.Renderer.VulkanDepth;
import Kairo.Renderer.VulkanDescriptor;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.VulkanSwapchain;
import Kairo.Foundation.Math;

export namespace kairo::renderer
{
    /// Owns the current swapchain render pass and two pipelines: the static
    /// depth-tested showcase cube and dynamic world-space debug lines. It is
    /// intentionally a small rendering unit, not a general render graph.
    class VulkanTriangle final
    {
    public:
        VulkanTriangle(const VulkanDevice& device, const VulkanSwapchain& swapchain)
            : m_VulkanDevice(device), m_Device(device.Handle()), m_ShowcaseMesh(Mesh::MakeCube()),
              m_VertexBuffer(device, m_ShowcaseMesh.VertexBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT),
              m_IndexBuffer(device, m_ShowcaseMesh.IndexBytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT),
              m_UniformBuffer(device, sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
              m_UniformDescriptor(device, m_UniformBuffer, sizeof(CameraUniform)), m_Depth(device, swapchain.Extent())
        {
            m_VertexBuffer.Write(m_ShowcaseMesh.Vertices().data(), m_ShowcaseMesh.VertexBytes());
            m_IndexBuffer.Write(m_ShowcaseMesh.Indices().data(), m_ShowcaseMesh.IndexBytes());
            Create(swapchain);
        }

        ~VulkanTriangle() { Destroy(); }
        VulkanTriangle(const VulkanTriangle&) = delete;
        VulkanTriangle& operator=(const VulkanTriangle&) = delete;

        /// Input: frame-local, renderer-neutral world-space debug lines.
        /// Output: copies CPU vertices for upload by the next DrawFrame.
        /// Task: accept physics/game debug output without creating a dependency
        /// from those systems to Vulkan. Calls must occur on the render thread.
        void SetDebugDrawList(const DebugDrawList& debug)
        {
            m_DebugVertices.clear();
            m_DebugVertices.reserve(debug.Lines().size() * 2u);
            for (const DebugLine& line : debug.Lines())
            {
                m_DebugVertices.push_back(MakeDebugVertex(line.A, line.Color));
                m_DebugVertices.push_back(MakeDebugVertex(line.B, line.Color));
            }
        }

        /// Task: rebuild resources whose compatibility depends on the swapchain.
        void Recreate(const VulkanSwapchain& swapchain)
        {
            Destroy();
            m_Depth.Recreate(swapchain.Extent());
            Create(swapchain);
        }

        /// Task: release image-view-dependent resources before swapchain teardown.
        void ReleaseSwapchainResources() noexcept { Destroy(); }

        /// Precondition: imageIndex belongs to the current swapchain and the
        /// caller has waited for this command buffer's completion fence.
        /// Task: render the showcase mesh, then debug lines into one depth pass.
        void Record(VulkanCommandBuffer& command, std::uint32_t imageIndex, VkExtent2D extent)
        {
            m_Camera.Advance(1.0f / 60.0f);
            UpdateUniform(extent);
            UploadDebugVertices();

            command.Begin();
            const std::array<VkClearValue, 2> clear{ VkClearValue{ { { 0.025f, 0.055f, 0.11f, 1.0f } } }, VkClearValue{ { 1.0f, 0u } } };
            VkRenderPassBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = m_RenderPass;
            begin.framebuffer = m_Framebuffers.at(imageIndex);
            begin.renderArea.extent = extent;
            begin.clearValueCount = static_cast<std::uint32_t>(clear.size());
            begin.pClearValues = clear.data();
            vkCmdBeginRenderPass(command.Handle(), &begin, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.extent = extent;
            vkCmdSetViewport(command.Handle(), 0u, 1u, &viewport);
            vkCmdSetScissor(command.Handle(), 0u, 1u, &scissor);

            BindCamera(command);
            vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_MeshPipeline);
            const VkBuffer meshBuffer = m_VertexBuffer.Handle();
            constexpr VkDeviceSize meshOffset = 0u;
            vkCmdBindVertexBuffers(command.Handle(), 0u, 1u, &meshBuffer, &meshOffset);
            vkCmdBindIndexBuffer(command.Handle(), m_IndexBuffer.Handle(), 0u, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command.Handle(), static_cast<std::uint32_t>(m_ShowcaseMesh.Indices().size()), 1u, 0u, 0, 0u);
            DrawDebugLines(command);
            vkCmdEndRenderPass(command.Handle());
            command.End();
        }

    private:
        struct CameraUniform final { std::array<float, 48> Values{}; };
        struct DebugVertex final
        {
            float Position[3]{};
            float Color[4]{};
        };

        const VulkanDevice& m_VulkanDevice;
        VkDevice m_Device = VK_NULL_HANDLE;
        VkRenderPass m_RenderPass = VK_NULL_HANDLE;
        VkPipelineLayout m_Layout = VK_NULL_HANDLE;
        VkPipeline m_MeshPipeline = VK_NULL_HANDLE;
        VkPipeline m_DebugLinePipeline = VK_NULL_HANDLE;
        Mesh m_ShowcaseMesh;
        VulkanHostBuffer m_VertexBuffer;
        VulkanHostBuffer m_IndexBuffer;
        VulkanHostBuffer m_UniformBuffer;
        VulkanUniformDescriptor m_UniformDescriptor;
        VulkanDepthAttachment m_Depth;
        ShowcaseCamera m_Camera;
        std::vector<VkFramebuffer> m_Framebuffers;
        std::vector<DebugVertex> m_DebugVertices;
        std::unique_ptr<VulkanHostBuffer> m_DebugVertexBuffer;
        VkDeviceSize m_DebugVertexCapacity = 0u;

        [[nodiscard]] static DebugVertex MakeDebugVertex(const kairo::foundation::math::Vec3f& position, DebugColor color) noexcept
        {
            return { { position.x, position.y, position.z }, { color.R, color.G, color.B, color.A } };
        }

        void Create(const VulkanSwapchain& swapchain)
        {
            try { CreateRenderPass(swapchain.Format()); CreateFramebuffers(swapchain); CreatePipelines(); }
            catch (...) { Destroy(); throw; }
        }

        void CreateRenderPass(VkFormat format)
        {
            VkAttachmentDescription color{};
            color.format = format; color.samples = VK_SAMPLE_COUNT_1_BIT; color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE; color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentDescription depth{};
            depth.format = m_Depth.Format(); depth.samples = VK_SAMPLE_COUNT_1_BIT; depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference colorReference{ 0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkAttachmentReference depthReference{ 1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1u;
            subpass.pColorAttachments = &colorReference; subpass.pDepthStencilAttachment = &depthReference;
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0u;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            const std::array attachments{ color, depth };
            VkRenderPassCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; create.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            create.pAttachments = attachments.data(); create.subpassCount = 1u; create.pSubpasses = &subpass;
            create.dependencyCount = 1u; create.pDependencies = &dependency;
            if (vkCreateRenderPass(m_Device, &create, nullptr, &m_RenderPass) != VK_SUCCESS) throw std::runtime_error("vkCreateRenderPass failed.");
        }

        void CreateFramebuffers(const VulkanSwapchain& swapchain)
        {
            for (const VkImageView imageView : swapchain.ImageViews())
            {
                const std::array attachments{ imageView, m_Depth.View() };
                VkFramebufferCreateInfo create{};
                create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; create.renderPass = m_RenderPass;
                create.attachmentCount = static_cast<std::uint32_t>(attachments.size()); create.pAttachments = attachments.data();
                create.width = swapchain.Extent().width; create.height = swapchain.Extent().height; create.layers = 1u;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(m_Device, &create, nullptr, &framebuffer) != VK_SUCCESS) throw std::runtime_error("vkCreateFramebuffer failed.");
                m_Framebuffers.push_back(framebuffer);
            }
        }

        void CreatePipelines()
        {
            VkPipelineLayoutCreateInfo layout{};
            layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; layout.setLayoutCount = 1u;
            const VkDescriptorSetLayout descriptorLayout = m_UniformDescriptor.Layout(); layout.pSetLayouts = &descriptorLayout;
            if (vkCreatePipelineLayout(m_Device, &layout, nullptr, &m_Layout) != VK_SUCCESS) throw std::runtime_error("vkCreatePipelineLayout failed.");

            VkVertexInputBindingDescription meshBinding{};
            meshBinding.binding = 0u; meshBinding.stride = sizeof(MeshVertex); meshBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            const std::array meshAttributes{
                VkVertexInputAttributeDescription{ 0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, Position) },
                VkVertexInputAttributeDescription{ 1u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, Color) }
            };
            VkPipelineVertexInputStateCreateInfo meshInput{};
            meshInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            meshInput.vertexBindingDescriptionCount = 1u; meshInput.pVertexBindingDescriptions = &meshBinding;
            meshInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(meshAttributes.size()); meshInput.pVertexAttributeDescriptions = meshAttributes.data();
            m_MeshPipeline = CreatePipeline("triangle.vert.spv", "triangle.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, meshInput, VK_TRUE);

            VkVertexInputBindingDescription binding{};
            binding.binding = 0u; binding.stride = sizeof(DebugVertex); binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            const std::array attributes{
                VkVertexInputAttributeDescription{ 0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugVertex, Position) },
                VkVertexInputAttributeDescription{ 1u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DebugVertex, Color) }
            };
            VkPipelineVertexInputStateCreateInfo lineInput{};
            lineInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; lineInput.vertexBindingDescriptionCount = 1u;
            lineInput.pVertexBindingDescriptions = &binding; lineInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
            lineInput.pVertexAttributeDescriptions = attributes.data();
            m_DebugLinePipeline = CreatePipeline("debug_line.vert.spv", "debug_line.frag.spv", VK_PRIMITIVE_TOPOLOGY_LINE_LIST, lineInput, VK_FALSE);
        }

        [[nodiscard]] VkPipeline CreatePipeline(const std::string& vertexName, const std::string& fragmentName,
            VkPrimitiveTopology topology, const VkPipelineVertexInputStateCreateInfo& vertexInput, VkBool32 depthWrite) const
        {
            const VkShaderModule vertex = CreateShaderModule(ReadSpirv(vertexName));
            const VkShaderModule fragment = CreateShaderModule(ReadSpirv(fragmentName));
            try
            {
                const VkPipeline pipeline = CreateGraphicsPipeline(vertex, fragment, topology, vertexInput, depthWrite);
                vkDestroyShaderModule(m_Device, fragment, nullptr); vkDestroyShaderModule(m_Device, vertex, nullptr);
                return pipeline;
            }
            catch (...)
            {
                vkDestroyShaderModule(m_Device, fragment, nullptr); vkDestroyShaderModule(m_Device, vertex, nullptr); throw;
            }
        }

        [[nodiscard]] VkPipeline CreateGraphicsPipeline(VkShaderModule vertex, VkShaderModule fragment, VkPrimitiveTopology topology,
            const VkPipelineVertexInputStateCreateInfo& vertexInput, VkBool32 depthWrite) const
        {
            VkPipelineShaderStageCreateInfo vertexStage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT; vertexStage.module = vertex; vertexStage.pName = "main";
            VkPipelineShaderStageCreateInfo fragmentStage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT; fragmentStage.module = fragment; fragmentStage.pName = "main";
            const std::array stages{ vertexStage, fragmentStage };
            VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            assembly.topology = topology;
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = 1u; viewport.scissorCount = 1u;
            VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL; rasterizer.cullMode = VK_CULL_MODE_NONE; rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rasterizer.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = depthWrite; depth.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            blend.attachmentCount = 1u; blend.pAttachments = &attachment;
            const std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()); dynamic.pDynamicStates = dynamicStates.data();
            VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            create.stageCount = static_cast<std::uint32_t>(stages.size()); create.pStages = stages.data(); create.pVertexInputState = &vertexInput;
            create.pInputAssemblyState = &assembly; create.pViewportState = &viewport; create.pRasterizationState = &rasterizer;
            create.pMultisampleState = &multisampling; create.pDepthStencilState = &depth; create.pColorBlendState = &blend;
            create.pDynamicState = &dynamic; create.layout = m_Layout; create.renderPass = m_RenderPass;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1u, &create, nullptr, &pipeline) != VK_SUCCESS) throw std::runtime_error("vkCreateGraphicsPipelines failed.");
            return pipeline;
        }

        void BindCamera(const VulkanCommandBuffer& command) const
        {
            const VkDescriptorSet descriptor = m_UniformDescriptor.Set();
            vkCmdBindDescriptorSets(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_Layout, 0u, 1u, &descriptor, 0u, nullptr);
        }

        void UploadDebugVertices()
        {
            if (m_DebugVertices.empty()) return;
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_DebugVertices.size() * sizeof(DebugVertex));
            if (bytes > m_DebugVertexCapacity)
            {
                VkDeviceSize capacity = m_DebugVertexCapacity == 0u ? 4096u : m_DebugVertexCapacity;
                while (capacity < bytes) capacity *= 2u;
                m_DebugVertexBuffer = std::make_unique<VulkanHostBuffer>(m_VulkanDevice, capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                m_DebugVertexCapacity = capacity;
            }
            m_DebugVertexBuffer->Write(m_DebugVertices.data(), bytes);
        }

        void DrawDebugLines(const VulkanCommandBuffer& command) const
        {
            if (m_DebugVertices.empty()) return;
            const VkBuffer buffer = m_DebugVertexBuffer->Handle();
            constexpr VkDeviceSize offset = 0u;
            vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_DebugLinePipeline);
            vkCmdBindVertexBuffers(command.Handle(), 0u, 1u, &buffer, &offset);
            vkCmdDraw(command.Handle(), static_cast<std::uint32_t>(m_DebugVertices.size()), 1u, 0u, 0u);
        }

        void UpdateUniform(VkExtent2D extent)
        {
            CameraUniform uniform{};
            CopyTranspose(m_Camera.Model(), uniform.Values.data()); CopyTranspose(m_Camera.View(), uniform.Values.data() + 16u);
            CopyTranspose(m_Camera.Projection(extent.width, extent.height), uniform.Values.data() + 32u); m_UniformBuffer.Write(&uniform, sizeof(uniform));
        }

        static void CopyTranspose(const kairo::foundation::math::Mat4f& matrix, float* destination) noexcept
        {
            for (std::size_t row = 0u; row < 4u; ++row) for (std::size_t column = 0u; column < 4u; ++column) destination[column * 4u + row] = matrix(row, column);
        }

        [[nodiscard]] VkShaderModule CreateShaderModule(const std::vector<std::uint32_t>& spirv) const
        {
            VkShaderModuleCreateInfo create{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            create.codeSize = spirv.size() * sizeof(std::uint32_t); create.pCode = spirv.data(); VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(m_Device, &create, nullptr, &module) != VK_SUCCESS) throw std::runtime_error("vkCreateShaderModule failed.");
            return module;
        }

        [[nodiscard]] static std::vector<std::uint32_t> ReadSpirv(const std::string& fileName)
        {
            const std::filesystem::path path = std::filesystem::path(KAIRO_RENDERER_SHADER_DIR) / fileName;
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) throw std::runtime_error("Cannot open shader binary: " + path.string());
            const std::streamsize bytes = input.tellg();
            if (bytes <= 0 || bytes % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0) throw std::runtime_error("Shader binary is empty or not aligned to 32-bit SPIR-V words: " + path.string());
            std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t)); input.seekg(0);
            if (!input.read(reinterpret_cast<char*>(words.data()), bytes)) throw std::runtime_error("Failed to read shader binary: " + path.string());
            return words;
        }

        void Destroy() noexcept
        {
            m_DebugVertexBuffer.reset(); m_DebugVertexCapacity = 0u;
            for (const VkFramebuffer framebuffer : m_Framebuffers) vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
            m_Framebuffers.clear();
            if (m_DebugLinePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_DebugLinePipeline, nullptr);
            if (m_MeshPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_MeshPipeline, nullptr);
            if (m_Layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
            if (m_RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            m_DebugLinePipeline = VK_NULL_HANDLE; m_MeshPipeline = VK_NULL_HANDLE; m_Layout = VK_NULL_HANDLE; m_RenderPass = VK_NULL_HANDLE;
        }
    };
}
