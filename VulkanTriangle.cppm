module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

export module Kairo.Renderer.VulkanTriangle;

import Kairo.Renderer.Camera;
import Kairo.Renderer.DebugDraw;
import Kairo.Renderer.Material;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.RenderScene;
import Kairo.Renderer.Texture;
import Kairo.Renderer.Types;
import Kairo.Renderer.ShadowSettings;
import Kairo.Renderer.VulkanBuffer;
import Kairo.Renderer.VulkanCommand;
import Kairo.Renderer.VulkanDescriptor;
import Kairo.Renderer.VulkanDevice;
import Kairo.Renderer.VulkanTexture;
import Kairo.Renderer.VulkanSwapchain;
import Kairo.Renderer.VulkanShadowMap;
import Kairo.Renderer.VulkanViewportTarget;
import Kairo.Renderer.VulkanBackendContext;
import Kairo.Foundation.Math;
import Kairo.Assets.TextureArtifact;

export namespace kairo::renderer
{
    /// Owns the current swapchain render pass, renderer-owned indexed mesh
    /// buffers, a lit mesh pipeline, and dynamic world-space debug lines. It
    /// remains a focused forward-pass unit rather than a general render graph.
    class VulkanTriangle final
    {
    public:
        VulkanTriangle(const VulkanDevice& device, const VulkanSwapchain& swapchain)
            : m_VulkanDevice(device), m_Device(device.Handle()),
              m_UniformBuffer(device, sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
              m_ShadowMap(device, 2048u),
              m_MaterialDescriptors(device),
              m_Viewport(device, swapchain.Extent())
        {
            m_FallbackWhite = std::make_unique<VulkanTexture2D>(device,
                SolidTexture(255u, 255u, 255u, 255u));
            m_FallbackNormal = std::make_unique<VulkanTexture2D>(device,
                SolidTexture(128u, 128u, 255u, 255u));
            m_FallbackBlack = std::make_unique<VulkanTexture2D>(device,
                SolidTexture(0u, 0u, 0u, 255u));
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

        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
            if (m_NextMesh == InvalidMeshHandle) throw std::overflow_error("Renderer mesh handle space is exhausted.");
            auto vertices = std::make_unique<VulkanHostBuffer>(m_VulkanDevice, mesh.VertexBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            auto indices = std::make_unique<VulkanHostBuffer>(m_VulkanDevice, mesh.IndexBytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            vertices->Write(mesh.Vertices().data(), mesh.VertexBytes());
            indices->Write(mesh.Indices().data(), mesh.IndexBytes());
            const MeshHandle handle = m_NextMesh++;
            m_Meshes.emplace(handle, GpuMesh{ std::move(vertices), std::move(indices), static_cast<std::uint32_t>(mesh.Indices().size()) });
            return handle;
        }

        [[nodiscard]] TextureHandle CreateTexture(
            const kairo::assets::TextureArtifactData& texture,
            TextureSampler sampling = {})
        {
            if (m_NextTexture == InvalidTextureHandle)
                throw std::overflow_error("Renderer texture handle space is exhausted.");
            const TextureHandle handle = m_NextTexture++;
            m_Textures.emplace(handle,
                std::make_unique<VulkanTexture2D>(m_VulkanDevice, texture, sampling));
            return handle;
        }

        void DestroyTexture(TextureHandle texture)
        {
            if (texture == InvalidTextureHandle || !m_Textures.contains(texture))
                throw std::out_of_range("Renderer does not own this texture handle.");
            const auto uses = [texture](const PBRMaterial& material)
            {
                return material.BaseColorTexture == texture ||
                    material.NormalTexture == texture ||
                    material.MetallicRoughnessTexture == texture ||
                    material.EmissiveTexture == texture ||
                    material.OcclusionTexture == texture;
            };
            if (std::ranges::any_of(m_Draws,
                [&](const MeshDraw& draw) { return uses(draw.Material); }) ||
                m_Environment.EnvironmentTexture == texture)
                throw std::logic_error(
                    "Cannot destroy a texture referenced by the submitted render scene.");
            m_Textures.erase(texture);
            m_DescriptorsDirty = true;
        }

        void DestroyMesh(MeshHandle mesh)
        {
            if (mesh == InvalidMeshHandle || m_Meshes.erase(mesh) == 0u) throw std::out_of_range("Renderer does not own this mesh handle.");
            std::erase_if(m_Draws, [mesh](const MeshDraw& draw) { return draw.Mesh == mesh; });
        }

        void SetRenderScene(const RenderScene& scene)
        {
            const auto validateTexture = [this](TextureHandle texture)
            {
                if (texture != InvalidTextureHandle && !m_Textures.contains(texture))
                    throw std::out_of_range(
                        "RenderScene material references an unknown texture handle.");
            };
            for (const MeshDraw& draw : scene.Draws())
            {
                RenderScene::Validate(draw);
                if (!m_Meshes.contains(draw.Mesh)) throw std::out_of_range("RenderScene references an unknown mesh handle.");
                validateTexture(draw.Material.BaseColorTexture);
                validateTexture(draw.Material.NormalTexture);
                validateTexture(draw.Material.MetallicRoughnessTexture);
                validateTexture(draw.Material.EmissiveTexture);
                validateTexture(draw.Material.OcclusionTexture);
            }
            validateTexture(scene.Environment().EnvironmentTexture);
            m_Draws = scene.Draws();
            m_Lights = scene.Lights();
            m_Environment = scene.Environment();
            m_DescriptorsDirty = true;
        }

        /// Task: accept a host-controlled scene camera while retaining matrix
        /// upload and native rendering ownership inside the Vulkan backend.
        void SetCameraPose(const CameraPose& pose) { m_Camera.SetPose(pose); }

        /// Task: rebuild resources whose compatibility depends on the swapchain.
        void Recreate(const VulkanSwapchain& swapchain)
        {
            Destroy();
            Create(swapchain);
        }

        /// Task: release image-view-dependent resources before swapchain teardown.
        void ReleaseSwapchainResources() noexcept { Destroy(); }

        [[nodiscard]] VkRenderPass RenderPass() const noexcept { return m_RenderPass; }

        [[nodiscard]] VulkanViewportTexture ViewportTexture() const noexcept
        {
            return m_Viewport.Texture();
        }

        /// Precondition: the renderer has idled the device. Scene pipelines are
        /// render-pass compatible across viewport sizes, so only attachment
        /// storage and the framebuffer are rebuilt.
        void ResizeViewport(VkExtent2D extent) { m_Viewport.Resize(extent); }

        /// Input: validated user-facing directional shadow controls.
        /// Task: update bias and visibility policy without rebuilding GPU
        /// resources. The next recorded frame observes the new values.
        void SetDirectionalShadowSettings(const DirectionalShadowSettings& settings)
        {
            settings.Validate();
            m_ShadowSettings = settings;
        }

        [[nodiscard]] const DirectionalShadowSettings& DirectionalShadows() const noexcept
        {
            return m_ShadowSettings;
        }

        void SetViewportShadingMode(ViewportShadingMode mode) noexcept { m_ShadingMode = mode; }
        [[nodiscard]] ViewportShadingMode ViewportShading() const noexcept { return m_ShadingMode; }

        /// Precondition: imageIndex belongs to the current swapchain and the
        /// caller has waited for this command buffer's completion fence.
        /// Task: render the scene into the sampled editor viewport, then record
        /// tooling UI into the swapchain presentation pass.
        void Record(VulkanCommandBuffer& command, std::uint32_t imageIndex, VkExtent2D extent,
            const VulkanOverlayRecorder& overlayRecorder,
            VkBuffer pickDestination = VK_NULL_HANDLE,
            std::optional<VkOffset2D> pickPixel = std::nullopt,
            VkBuffer captureDestination = VK_NULL_HANDLE)
        {
            RebuildMaterialDescriptors();
            UpdateUniform(m_Viewport.Extent());
            UploadDebugVertices();

            command.Begin();
            // Populate and transition the sampled depth image whenever a mesh
            // pass can consume its descriptor. Enabled controls attenuation,
            // not resource validity, so toggling it before frame one is safe.
            if (!m_Draws.empty() && ShadowLightIndex().has_value())
                DrawDirectionalShadowMap(command);
            VkClearValue colorClear{};
            colorClear.color = { { m_Environment.BackgroundColor.x,
                m_Environment.BackgroundColor.y,
                m_Environment.BackgroundColor.z, 1.0f } };
            VkClearValue objectClear{};
            objectClear.color.uint32[0] = 0u;
            VkClearValue depthClear{};
            depthClear.depthStencil = { 1.0f, 0u };
            const std::array sceneClear{ colorClear, objectClear, depthClear };
            VkRenderPassBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = m_Viewport.RenderPass();
            begin.framebuffer = m_Viewport.Framebuffer();
            begin.renderArea.extent = m_Viewport.Extent();
            begin.clearValueCount = static_cast<std::uint32_t>(sceneClear.size());
            begin.pClearValues = sceneClear.data();
            vkCmdBeginRenderPass(command.Handle(), &begin, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width = static_cast<float>(m_Viewport.Extent().width);
            viewport.height = static_cast<float>(m_Viewport.Extent().height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.extent = m_Viewport.Extent();
            vkCmdSetViewport(command.Handle(), 0u, 1u, &viewport);
            vkCmdSetScissor(command.Handle(), 0u, 1u, &scissor);

            DrawMeshes(command);
            DrawDebugLines(command);
            vkCmdEndRenderPass(command.Handle());

            if (pickDestination != VK_NULL_HANDLE && pickPixel.has_value())
            {
                VkBufferImageCopy copy{};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                copy.imageOffset = { pickPixel->x, pickPixel->y, 0 };
                copy.imageExtent = { 1u, 1u, 1u };
                vkCmdCopyImageToBuffer(command.Handle(), m_Viewport.ObjectIDImage(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pickDestination, 1u, &copy);
            }
            if (captureDestination != VK_NULL_HANDLE)
            {
                VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.image = m_Viewport.ColorImage();
                toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u };
                vkCmdPipelineBarrier(command.Handle(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);

                VkBufferImageCopy copy{};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                copy.imageExtent = { m_Viewport.Extent().width, m_Viewport.Extent().height, 1u };
                vkCmdCopyImageToBuffer(command.Handle(), m_Viewport.ColorImage(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureDestination, 1u, &copy);

                toTransfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toTransfer.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toTransfer.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(command.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);
            }

            const VkClearValue presentationClear{ { { 0.018f, 0.021f, 0.027f, 1.0f } } };
            begin.renderPass = m_RenderPass;
            begin.framebuffer = m_Framebuffers.at(imageIndex);
            begin.renderArea.extent = extent;
            begin.clearValueCount = 1u;
            begin.pClearValues = &presentationClear;
            vkCmdBeginRenderPass(command.Handle(), &begin, VK_SUBPASS_CONTENTS_INLINE);
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            scissor.extent = extent;
            vkCmdSetViewport(command.Handle(), 0u, 1u, &viewport);
            vkCmdSetScissor(command.Handle(), 0u, 1u, &scissor);
            if (overlayRecorder) overlayRecorder(command.Handle());
            vkCmdEndRenderPass(command.Handle());
            command.End();
        }

    private:
        /// std140-compatible view/projection plus directional-light data.
        struct CameraUniform final { std::array<float, 332> Values{}; };
        /// 64-byte model + three padded normal columns + 16-byte tint exactly
        /// fit Vulkan's guaranteed minimum 128-byte push-constant capacity.
        struct MeshPushConstants final { std::array<float, 32> Values{}; };
        struct GpuMesh final
        {
            std::unique_ptr<VulkanHostBuffer> Vertices;
            std::unique_ptr<VulkanHostBuffer> Indices;
            std::uint32_t IndexCount = 0u;
        };
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
        VkPipeline m_TransparentPipeline = VK_NULL_HANDLE;
        VkPipeline m_DebugLinePipeline = VK_NULL_HANDLE;
        VkPipeline m_ShadowPipeline = VK_NULL_HANDLE;
        VulkanHostBuffer m_UniformBuffer;
        VulkanDirectionalShadowMap m_ShadowMap;
        std::unique_ptr<VulkanTexture2D> m_FallbackWhite;
        std::unique_ptr<VulkanTexture2D> m_FallbackNormal;
        std::unique_ptr<VulkanTexture2D> m_FallbackBlack;
        VulkanMaterialDescriptors m_MaterialDescriptors;
        VulkanViewportTarget m_Viewport;
        ShowcaseCamera m_Camera;
        DirectionalShadowSettings m_ShadowSettings;
        ViewportShadingMode m_ShadingMode = ViewportShadingMode::Lit;
        std::vector<VkFramebuffer> m_Framebuffers;
        std::unordered_map<MeshHandle, GpuMesh> m_Meshes;
        std::unordered_map<TextureHandle, std::unique_ptr<VulkanTexture2D>> m_Textures;
        std::vector<MeshDraw> m_Draws;
        std::vector<RenderLight> m_Lights;
        RenderEnvironment m_Environment;
        MeshHandle m_NextMesh = 1u;
        TextureHandle m_NextTexture = 1u;
        bool m_DescriptorsDirty = true;
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
            VkAttachmentReference colorReference{ 0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount = 1u;
            subpass.pColorAttachments = &colorReference;
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0u;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; create.attachmentCount = 1u;
            create.pAttachments = &color; create.subpassCount = 1u; create.pSubpasses = &subpass;
            create.dependencyCount = 1u; create.pDependencies = &dependency;
            if (vkCreateRenderPass(m_Device, &create, nullptr, &m_RenderPass) != VK_SUCCESS) throw std::runtime_error("vkCreateRenderPass failed.");
        }

        void CreateFramebuffers(const VulkanSwapchain& swapchain)
        {
            for (const VkImageView imageView : swapchain.ImageViews())
            {
                VkFramebufferCreateInfo create{};
                create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; create.renderPass = m_RenderPass;
                create.attachmentCount = 1u; create.pAttachments = &imageView;
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
            const VkDescriptorSetLayout descriptorLayout = m_MaterialDescriptors.Layout(); layout.pSetLayouts = &descriptorLayout;
            const VkPushConstantRange pushConstants{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0u, sizeof(MeshPushConstants) };
            layout.pushConstantRangeCount = 1u; layout.pPushConstantRanges = &pushConstants;
            if (vkCreatePipelineLayout(m_Device, &layout, nullptr, &m_Layout) != VK_SUCCESS) throw std::runtime_error("vkCreatePipelineLayout failed.");

            VkVertexInputBindingDescription meshBinding{};
            meshBinding.binding = 0u; meshBinding.stride = sizeof(MeshVertex); meshBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            const std::array meshAttributes{
                VkVertexInputAttributeDescription{ 0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, Position) },
                VkVertexInputAttributeDescription{ 1u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, Color) },
                VkVertexInputAttributeDescription{ 2u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, Normal) },
                VkVertexInputAttributeDescription{ 3u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, TexCoord) }
            };
            VkPipelineVertexInputStateCreateInfo meshInput{};
            meshInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            meshInput.vertexBindingDescriptionCount = 1u; meshInput.pVertexBindingDescriptions = &meshBinding;
            meshInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(meshAttributes.size()); meshInput.pVertexAttributeDescriptions = meshAttributes.data();
            m_MeshPipeline = CreatePipeline("triangle.vert.spv", "triangle.frag.spv",
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, meshInput, VK_TRUE, false);
            m_TransparentPipeline = CreatePipeline("triangle.vert.spv", "triangle.frag.spv",
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, meshInput, VK_FALSE, true);
            m_ShadowPipeline = CreateShadowPipeline(meshInput);

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
            m_DebugLinePipeline = CreatePipeline("debug_line.vert.spv", "debug_line.frag.spv",
                VK_PRIMITIVE_TOPOLOGY_LINE_LIST, lineInput, VK_FALSE, false);
        }

        /// Output: a vertex-only depth pipeline compatible with the persistent
        /// directional shadow render pass.
        /// Task: reuse the scene mesh layout and push constants while omitting
        /// all color attachments and fragment work. Dynamic depth bias allows
        /// tools to tune acne/peter-panning tradeoffs without pipeline rebuilds.
        [[nodiscard]] VkPipeline CreateShadowPipeline(const VkPipelineVertexInputStateCreateInfo& vertexInput) const
        {
            const VkShaderModule vertex = CreateShaderModule(ReadSpirv("shadow.vert.spv"));
            try
            {
                VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
                stage.module = vertex;
                stage.pName = "main";
                VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
                assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
                viewport.viewportCount = 1u;
                viewport.scissorCount = 1u;
                VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
                rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
                rasterizer.cullMode = VK_CULL_MODE_NONE;
                rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                rasterizer.depthBiasEnable = VK_TRUE;
                rasterizer.lineWidth = 1.0f;
                VkPipelineMultisampleStateCreateInfo multisampling{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
                multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
                depth.depthTestEnable = VK_TRUE;
                depth.depthWriteEnable = VK_TRUE;
                depth.depthCompareOp = VK_COMPARE_OP_LESS;
                VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
                const std::array dynamicStates{
                    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS
                };
                VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
                dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
                dynamic.pDynamicStates = dynamicStates.data();
                VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
                create.stageCount = 1u;
                create.pStages = &stage;
                create.pVertexInputState = &vertexInput;
                create.pInputAssemblyState = &assembly;
                create.pViewportState = &viewport;
                create.pRasterizationState = &rasterizer;
                create.pMultisampleState = &multisampling;
                create.pDepthStencilState = &depth;
                create.pColorBlendState = &blend;
                create.pDynamicState = &dynamic;
                create.layout = m_Layout;
                create.renderPass = m_ShadowMap.RenderPass();
                VkPipeline pipeline = VK_NULL_HANDLE;
                if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1u, &create, nullptr, &pipeline) != VK_SUCCESS)
                    throw std::runtime_error("vkCreateGraphicsPipelines for the directional shadow pass failed.");
                vkDestroyShaderModule(m_Device, vertex, nullptr);
                return pipeline;
            }
            catch (...)
            {
                vkDestroyShaderModule(m_Device, vertex, nullptr);
                throw;
            }
        }

        [[nodiscard]] VkPipeline CreatePipeline(const std::string& vertexName, const std::string& fragmentName,
            VkPrimitiveTopology topology, const VkPipelineVertexInputStateCreateInfo& vertexInput,
            VkBool32 depthWrite, bool alphaBlend) const
        {
            const VkShaderModule vertex = CreateShaderModule(ReadSpirv(vertexName));
            const VkShaderModule fragment = CreateShaderModule(ReadSpirv(fragmentName));
            try
            {
                const VkPipeline pipeline = CreateGraphicsPipeline(vertex, fragment, topology,
                    vertexInput, depthWrite, alphaBlend);
                vkDestroyShaderModule(m_Device, fragment, nullptr); vkDestroyShaderModule(m_Device, vertex, nullptr);
                return pipeline;
            }
            catch (...)
            {
                vkDestroyShaderModule(m_Device, fragment, nullptr); vkDestroyShaderModule(m_Device, vertex, nullptr); throw;
            }
        }

        [[nodiscard]] VkPipeline CreateGraphicsPipeline(VkShaderModule vertex, VkShaderModule fragment, VkPrimitiveTopology topology,
            const VkPipelineVertexInputStateCreateInfo& vertexInput, VkBool32 depthWrite,
            bool alphaBlend) const
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
            if (alphaBlend)
            {
                attachment.blendEnable = VK_TRUE;
                attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.colorBlendOp = VK_BLEND_OP_ADD;
                attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            }
            VkPipelineColorBlendAttachmentState objectAttachment{};
            objectAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            const std::array blendAttachments{ attachment, objectAttachment };
            blend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
            blend.pAttachments = blendAttachments.data();
            const std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()); dynamic.pDynamicStates = dynamicStates.data();
            VkGraphicsPipelineCreateInfo create{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            create.stageCount = static_cast<std::uint32_t>(stages.size()); create.pStages = stages.data(); create.pVertexInputState = &vertexInput;
            create.pInputAssemblyState = &assembly; create.pViewportState = &viewport; create.pRasterizationState = &rasterizer;
            create.pMultisampleState = &multisampling; create.pDepthStencilState = &depth; create.pColorBlendState = &blend;
            create.pDynamicState = &dynamic; create.layout = m_Layout; create.renderPass = m_Viewport.RenderPass();
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1u, &create, nullptr, &pipeline) != VK_SUCCESS) throw std::runtime_error("vkCreateGraphicsPipelines failed.");
            return pipeline;
        }

        void BindMaterial(const VulkanCommandBuffer& command, std::size_t index) const
        {
            const VkDescriptorSet descriptor = m_MaterialDescriptors.Set(index);
            vkCmdBindDescriptorSets(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_Layout, 0u, 1u, &descriptor, 0u, nullptr);
        }

        /// Task: rasterize every submitted mesh from the directional light's
        /// orthographic camera. Only model transforms are consumed from the
        /// shared push block; material and normal slots remain zeroed.
        void DrawDirectionalShadowMap(const VulkanCommandBuffer& command) const
        {
            const VkClearValue clear{ { 1.0f, 0u } };
            VkRenderPassBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            begin.renderPass = m_ShadowMap.RenderPass();
            begin.framebuffer = m_ShadowMap.Framebuffer();
            begin.renderArea.extent = { m_ShadowMap.Resolution(), m_ShadowMap.Resolution() };
            begin.clearValueCount = 1u;
            begin.pClearValues = &clear;
            vkCmdBeginRenderPass(command.Handle(), &begin, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width = static_cast<float>(m_ShadowMap.Resolution());
            viewport.height = static_cast<float>(m_ShadowMap.Resolution());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            const VkRect2D scissor{ {}, { m_ShadowMap.Resolution(), m_ShadowMap.Resolution() } };
            vkCmdSetViewport(command.Handle(), 0u, 1u, &viewport);
            vkCmdSetScissor(command.Handle(), 0u, 1u, &scissor);
            vkCmdSetDepthBias(command.Handle(), m_ShadowSettings.ConstantDepthBias, 0.0f, m_ShadowSettings.SlopeDepthBias);
            BindMaterial(command, 0u);
            vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipeline);

            constexpr VkDeviceSize offset = 0u;
            for (const MeshDraw& draw : m_Draws)
            {
                if (!draw.CastShadows ||
                    draw.Material.AlphaMode == MaterialAlphaMode::Blend) continue;
                const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
                MeshPushConstants push{};
                CopyTranspose(draw.Model, push.Values.data());
                // Vulkan requires every stage declared by an overlapping
                // pipeline-layout range, even when this pipeline has no
                // fragment shader consuming the bytes.
                vkCmdPushConstants(command.Handle(), m_Layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0u, sizeof(push), &push);
                const VkBuffer vertexBuffer = mesh.Vertices->Handle();
                vkCmdBindVertexBuffers(command.Handle(), 0u, 1u, &vertexBuffer, &offset);
                vkCmdBindIndexBuffer(command.Handle(), mesh.Indices->Handle(), 0u, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(command.Handle(), mesh.IndexCount, 1u, 0u, 0, 0u);
            }
            vkCmdEndRenderPass(command.Handle());
        }

        void DrawMeshes(const VulkanCommandBuffer& command) const
        {
            if (m_Draws.empty()) return;
            constexpr VkDeviceSize offset = 0u;
            const auto drawOne = [&](std::size_t drawIndex)
            {
                const MeshDraw& draw = m_Draws[drawIndex];
                BindMaterial(command, drawIndex);
                const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
                MeshPushConstants push{};
                CopyTranspose(draw.Model, push.Values.data());
                const auto normal = ComputeNormalMatrix(draw.Model);
                for (std::size_t column = 0u; column < 3u; ++column)
                    for (std::size_t row = 0u; row < 3u; ++row)
                        push.Values[16u + column * 4u + row] = normal(row, column);
                push.Values[30u] = draw.ReceiveShadows ? 1.0f : 0.0f;
                push.Values[31u] = std::bit_cast<float>(draw.ObjectID);
                vkCmdPushConstants(command.Handle(), m_Layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0u, sizeof(push), &push);
                const VkBuffer vertexBuffer = mesh.Vertices->Handle();
                vkCmdBindVertexBuffers(command.Handle(), 0u, 1u, &vertexBuffer, &offset);
                vkCmdBindIndexBuffer(command.Handle(), mesh.Indices->Handle(), 0u,
                    VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(command.Handle(), mesh.IndexCount, 1u, 0u, 0, 0u);
            };

            vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_MeshPipeline);
            for (std::size_t index = 0u; index < m_Draws.size(); ++index)
                if (m_Draws[index].Material.AlphaMode != MaterialAlphaMode::Blend)
                    drawOne(index);

            std::vector<std::size_t> transparentDraws;
            transparentDraws.reserve(m_Draws.size());
            for (std::size_t index = 0u; index < m_Draws.size(); ++index)
                if (m_Draws[index].Material.AlphaMode == MaterialAlphaMode::Blend)
                    transparentDraws.push_back(index);
            using kairo::foundation::math::Dot;
            using kairo::foundation::math::ExtractTranslation;
            using kairo::foundation::math::Vec3f;
            const Vec3f cameraPosition = m_Camera.Position();
            std::stable_sort(transparentDraws.begin(), transparentDraws.end(),
                [&](std::size_t left, std::size_t right)
                {
                    const Vec3f leftOffset = ExtractTranslation(m_Draws[left].Model) -
                        cameraPosition;
                    const Vec3f rightOffset = ExtractTranslation(m_Draws[right].Model) -
                        cameraPosition;
                    return Dot(leftOffset, leftOffset) > Dot(rightOffset, rightOffset);
                });
            vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_TransparentPipeline);
            for (const std::size_t index : transparentDraws) drawOne(index);
        }

        [[nodiscard]] static kairo::assets::TextureArtifactData SolidTexture(
            std::uint8_t red, std::uint8_t green, std::uint8_t blue,
            std::uint8_t alpha)
        {
            kairo::assets::TextureArtifactData texture;
            texture.Format = kairo::assets::TexturePixelFormat::RGBA8;
            texture.ColorSpace = kairo::assets::TextureColorSpace::Linear;
            texture.Semantic = kairo::assets::TextureSemantic::Data;
            texture.Mips.push_back({ 1u, 1u, {
                std::byte{ red }, std::byte{ green }, std::byte{ blue },
                std::byte{ alpha } } });
            return texture;
        }

        [[nodiscard]] const VulkanTexture2D& TextureOr(
            TextureHandle handle, const VulkanTexture2D& fallback) const
        {
            if (handle == InvalidTextureHandle) return fallback;
            return *m_Textures.at(handle);
        }

        void RebuildMaterialDescriptors()
        {
            if (!m_DescriptorsDirty) return;
            std::vector<VulkanMaterialImages> bindings;
            bindings.reserve(m_Draws.size());
            for (const MeshDraw& draw : m_Draws)
            {
                const std::array<const VulkanTexture2D*, 5u> textures{
                    &TextureOr(draw.Material.BaseColorTexture, *m_FallbackWhite),
                    &TextureOr(draw.Material.NormalTexture, *m_FallbackNormal),
                    &TextureOr(draw.Material.MetallicRoughnessTexture, *m_FallbackWhite),
                    &TextureOr(draw.Material.EmissiveTexture, *m_FallbackWhite),
                    &TextureOr(draw.Material.OcclusionTexture, *m_FallbackWhite)
                };
                VulkanMaterialImages binding;
                binding.Material = draw.Material;
                for (std::size_t channel = 0u; channel < textures.size(); ++channel)
                {
                    binding.Views[channel] = textures[channel]->View();
                    binding.Samplers[channel] = textures[channel]->Sampler();
                }
                bindings.push_back(binding);
            }
            m_MaterialDescriptors.Rebuild(m_UniformBuffer, sizeof(CameraUniform),
                m_ShadowMap.View(), m_ShadowMap.Sampler(), bindings);
            m_DescriptorsDirty = false;
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
            using namespace kairo::foundation::math;
            CameraUniform uniform{};
            CopyTranspose(m_Camera.View(), uniform.Values.data());
            CopyTranspose(m_Camera.Projection(extent.width, extent.height), uniform.Values.data() + 16u);
            const auto shadowIndex = ShadowLightIndex();
            const Vec3f lightDirection = shadowIndex.has_value()
                ? SafeNormalize(m_Lights[*shadowIndex].Direction, Vec3f::Up())
                : Vec3f::Up();
            uniform.Values[32u] = lightDirection.x; uniform.Values[33u] = lightDirection.y;
            uniform.Values[34u] = lightDirection.z;
            uniform.Values[35u] = shadowIndex.has_value()
                ? m_Lights[*shadowIndex].Intensity : 0.0f;
            uniform.Values[36u] = m_Environment.AmbientColor.x * m_Environment.AmbientIntensity;
            uniform.Values[37u] = m_Environment.AmbientColor.y * m_Environment.AmbientIntensity;
            uniform.Values[38u] = m_Environment.AmbientColor.z * m_Environment.AmbientIntensity;
            uniform.Values[39u] = 1.0f;
            const auto cameraPosition = m_Camera.Position();
            uniform.Values[40u] = cameraPosition.x; uniform.Values[41u] = cameraPosition.y;
            uniform.Values[42u] = cameraPosition.z; uniform.Values[43u] = 1.0f;
            const Mat4f lightView = LookAt(lightDirection * 12.0f, Vec3f::Zero(), Vec3f::Up());
            Mat4f lightProjection = Orthographic(-8.0f, 8.0f, -8.0f, 8.0f, 0.1f, 30.0f);
            lightProjection(1u, 1u) *= -1.0f;
            CopyTranspose(lightProjection * lightView, uniform.Values.data() + 44u);
            uniform.Values[60u] = m_ShadowSettings.Enabled && shadowIndex.has_value()
                ? 1.0f : 0.0f;
            uniform.Values[61u] = m_ShadowSettings.Strength;
            uniform.Values[62u] = 1.0f / static_cast<float>(m_ShadowMap.Resolution());
            uniform.Values[63u] = m_ShadowSettings.ReceiverBias;
            uniform.Values[64u] = static_cast<float>(m_ShadingMode);
            uniform.Values[65u] = m_Environment.ExposureEV100;
            uniform.Values[68u] = static_cast<float>(m_Lights.size());
            uniform.Values[69u] = shadowIndex.has_value()
                ? static_cast<float>(*shadowIndex) : -1.0f;
            uniform.Values[72u] = m_Environment.BackgroundColor.x;
            uniform.Values[73u] = m_Environment.BackgroundColor.y;
            uniform.Values[74u] = m_Environment.BackgroundColor.z;
            uniform.Values[75u] = 1.0f;
            for (std::size_t index = 0u; index < m_Lights.size(); ++index)
            {
                const RenderLight& light = m_Lights[index];
                const std::size_t position = 76u + index * 4u;
                const std::size_t direction = 140u + index * 4u;
                const std::size_t color = 204u + index * 4u;
                const std::size_t spot = 268u + index * 4u;
                uniform.Values[position] = light.Position.x;
                uniform.Values[position + 1u] = light.Position.y;
                uniform.Values[position + 2u] = light.Position.z;
                uniform.Values[position + 3u] = static_cast<float>(light.Type);
                const Vec3f normalized = SafeNormalize(light.Direction, Vec3f::Up());
                uniform.Values[direction] = normalized.x;
                uniform.Values[direction + 1u] = normalized.y;
                uniform.Values[direction + 2u] = normalized.z;
                uniform.Values[direction + 3u] = light.Range;
                uniform.Values[color] = light.Color.x;
                uniform.Values[color + 1u] = light.Color.y;
                uniform.Values[color + 2u] = light.Color.z;
                uniform.Values[color + 3u] = light.Intensity;
                uniform.Values[spot] = std::cos(light.InnerConeRadians);
                uniform.Values[spot + 1u] = std::cos(light.OuterConeRadians);
                uniform.Values[spot + 2u] = light.AreaWidth;
                uniform.Values[spot + 3u] = light.AreaHeight;
            }
            m_UniformBuffer.Write(&uniform, sizeof(uniform));
        }

        [[nodiscard]] std::optional<std::size_t> ShadowLightIndex() const noexcept
        {
            for (std::size_t index = 0u; index < m_Lights.size(); ++index)
                if (m_Lights[index].Type == RenderLightType::Directional &&
                    m_Lights[index].CastShadows)
                    return index;
            return std::nullopt;
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
            if (m_ShadowPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
            if (m_DebugLinePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_DebugLinePipeline, nullptr);
            if (m_TransparentPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_Device, m_TransparentPipeline, nullptr);
            if (m_MeshPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_MeshPipeline, nullptr);
            if (m_Layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
            if (m_RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            m_ShadowPipeline = VK_NULL_HANDLE; m_DebugLinePipeline = VK_NULL_HANDLE;
            m_MeshPipeline = VK_NULL_HANDLE; m_TransparentPipeline = VK_NULL_HANDLE;
            m_Layout = VK_NULL_HANDLE; m_RenderPass = VK_NULL_HANDLE;
        }
    };
}
