module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "detail/DescriptorSceneInvalidation.hpp"
#include "detail/DrawBindingState.hpp"
#include "detail/InstanceRuns.hpp"
#include "detail/VulkanFrustumBounds.hpp"
#include "detail/StaticVisibilityBvh.hpp"
#include "detail/FenceRetiredResources.hpp"
#include "EmbeddedVulkanShaders.hpp"

// TEACHING DRAW PATH: renderer-owned MeshHandle/TextureHandle -> GPU buffers,
// VulkanTexture2D and descriptor sets -> native VkCommandBuffer recording.
// VulkanRenderer owns fence/acquire/submit; this module owns scene render
// passes, shadow/viewport attachments and sampled material bindings.
// See docs/TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md.
export module Kairo.Renderer.VulkanTriangle;

import Kairo.Renderer.Camera;
import Kairo.Renderer.DebugDraw;
import Kairo.Renderer.Material;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.Skinning;
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
    // Runtime owner of mesh/texture maps, PBR descriptor pool, shadow depth
    // target, viewport color/ID/depth images and graphics pipelines.
    // Its swapchain-specific resources must retire before swapchain rebuild;
    // the VulkanRenderer facade controls frame-level GPU idle/fence waits.
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
            if (mesh.RequiredJointCount() > MaximumSkinJoints)
                throw std::length_error("Vulkan skinned mesh exceeds the portable joint limit.");
            if (m_NextMesh == InvalidMeshHandle)
                throw std::overflow_error("Renderer mesh handle space is exhausted.");
            auto vertices = std::make_unique<VulkanHostBuffer>(m_VulkanDevice,
                mesh.VertexBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            auto indices = std::make_unique<VulkanHostBuffer>(m_VulkanDevice,
                mesh.IndexBytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            vertices->Write(mesh.Vertices().data(), mesh.VertexBytes());
            indices->Write(mesh.Indices().data(), mesh.IndexBytes());
            std::unique_ptr<VulkanHostBuffer> skin;
            if (mesh.IsSkinned())
            {
                skin = std::make_unique<VulkanHostBuffer>(m_VulkanDevice,
                    mesh.SkinBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                skin->Write(mesh.Skinning().data(), mesh.SkinBytes());
            }
            detail::LocalBounds bounds;
            for (const auto& vertex : mesh.Vertices()) bounds.Include(vertex.Position);
            const MeshHandle handle = m_NextMesh++;
            m_Meshes.emplace(handle, GpuMesh{ std::move(vertices), std::move(skin),
                std::move(indices), static_cast<std::uint32_t>(mesh.Indices().size()),
                mesh.RequiredJointCount(), bounds });
            return handle;
        }

        // Turn a validated portable texture artifact into one device-local
        // VulkanTexture2D. Keep its unique_ptr in the handle map; the
        // descriptor sets borrow native View()/Sampler() and do not own it.
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

        // Reject unknown handles and textures referenced by the CURRENT
        // submitted render scene/environment. RendererRuntime's native
        // deletion path must also retire earlier submitted work first.
        // Erasing a texture invalidates its image view/sampler descriptors.
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
            const auto found=m_Meshes.find(mesh);
            if (mesh == InvalidMeshHandle || found==m_Meshes.end())
                throw std::out_of_range("Renderer does not own this mesh handle.");
            // Keep the buffers alive for the previously submitted frame.
            // The bounded journal has reserved storage; failure before
            // moving leaves the handle and its draw records untouched.
            m_RetiredMeshes.Retire(std::move(found->second));
            m_Meshes.erase(found);
            // Descriptor sets are indexed by draw position. Removing even a
            // middle draw changes that mapping for the following draws.
            if (std::erase_if(m_Draws, [mesh](const MeshDraw& draw) { return draw.Mesh == mesh; }) != 0u)
            {
                m_DescriptorsDirty = true;
                InvalidateVisibilityTree();
            }
        }

        [[nodiscard]] bool MeshRetirementFull() const noexcept {return m_RetiredMeshes.Full();}
        // Caller must have proven the last frame completed, or device idle/lost.
        void ReleaseCompletedMeshes() noexcept {m_RetiredMeshes.ReleaseCompleted();}

        // Validate each draw's mesh ownership, skin palette coverage and
        // all five PBR texture handles before copying scene draw records.
        // Environment texture has separate validation. Only descriptor-backed
        // inputs invalidate the native generation: transform-only submissions
        // and camera/light changes use push constants or the frame uniform.
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
                if (!m_Meshes.contains(draw.Mesh))
                    throw std::out_of_range("RenderScene references an unknown mesh handle.");
                const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
                if (mesh.RequiredJoints > 0u)
                {
                    if (draw.Skinning.Size() < mesh.RequiredJoints)
                        throw std::invalid_argument(
                            "Vulkan skinned draw palette does not cover every referenced joint.");
                }
                else if (!draw.Skinning.Empty())
                    throw std::invalid_argument(
                        "Vulkan static mesh draw cannot carry a skin palette.");
                validateTexture(draw.Material.BaseColorTexture);
                validateTexture(draw.Material.NormalTexture);
                validateTexture(draw.Material.MetallicRoughnessTexture);
                validateTexture(draw.Material.EmissiveTexture);
                validateTexture(draw.Material.OcclusionTexture);
            }
            validateTexture(scene.Environment().EnvironmentTexture);
            // Latch before assigning potentially allocating vectors. A failed
            // publication or earlier native rebuild must not clear dirty.
            detail::InvalidateDescriptorInputs(m_DescriptorsDirty, m_Draws, scene.Draws(),
                m_Environment.EnvironmentTexture, scene.Environment().EnvironmentTexture);
            if (m_VisibilityWarm && !SameVisibilityGeometry(scene.Draws()))
                InvalidateVisibilityTree();
            m_Draws = scene.Draws();
            m_Lights = scene.Lights();
            m_Environment = scene.Environment();
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

        // Validate the whole submission before replacing the previous list.
        // Rectangles are CPU values, not per-rectangle GPU resources.
        void SetViewportOverlay(const std::vector<ViewportOverlayRect>& rects)
        {
            if(rects.size()>4096u)throw std::length_error("Viewport overlay rectangle limit.");
            for(const auto& rect:rects)
                for(float channel:rect.Color)
                    if(!std::isfinite(channel)||channel<0.f||channel>1.f)
                        throw std::invalid_argument("Invalid viewport overlay color.");
            m_ViewportOverlay=rects;
        }

        void SetViewportShadingMode(ViewportShadingMode mode) noexcept { m_ShadingMode = mode; }
        [[nodiscard]] ViewportShadingMode ViewportShading() const noexcept { return m_ShadingMode; }

        /// Precondition: imageIndex belongs to the current swapchain and the
        /// caller has waited for this command buffer's completion fence.
        /// Task: render the scene into the sampled editor viewport, then record
        /// tooling UI into the swapchain presentation pass.
        // Encode the REAL native shadow -> offscreen viewport -> optional
        // object-ID/capture copy -> swapchain presentation command sequence.
        // RebuildMaterialDescriptors must occur after a completed frame fence;
        // render-pass layout dependencies and explicit VkImageMemoryBarriers
        // (not the CPU RenderGraph alone) make GPU reads/copies legal.
        void Record(VulkanCommandBuffer& command, std::uint32_t imageIndex, VkExtent2D extent,
            const VulkanOverlayRecorder& overlayRecorder,
            VkBuffer pickDestination = VK_NULL_HANDLE,
            std::optional<VkOffset2D> pickPixel = std::nullopt,
            VkBuffer captureDestination = VK_NULL_HANDLE)
        {
            RebuildMaterialDescriptors();
            if(!overlayRecorder)RefreshPresentationDescriptor();
            UpdateUniform(m_Viewport.Extent());
            UploadDebugVertices();
            PrepareVisibility();
            PrepareInstances(); // prior frame fence has completed

            command.Begin();
            // Populate and transition the sampled depth image whenever a mesh
            // pass can consume its descriptor. Enabled controls attenuation,
            // not resource validity, so toggling it before frame one is safe.
            if (!m_Draws.empty())
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
            // Clear only color attachment zero, inside the scene render pass.
            // vkCmdClearAttachments ignores depth testing and does not alter
            // picking IDs. Capture and viewport sampling see the same UI.
            for(const auto& rect:m_ViewportOverlay)
            {
                const auto overlayExtent=m_Viewport.Extent();
                const auto x0=std::clamp<std::int64_t>(rect.X,0,overlayExtent.width);
                const auto y0=std::clamp<std::int64_t>(rect.Y,0,overlayExtent.height);
                const auto x1=std::clamp<std::int64_t>(std::int64_t(rect.X)+rect.Width,0,overlayExtent.width);
                const auto y1=std::clamp<std::int64_t>(std::int64_t(rect.Y)+rect.Height,0,overlayExtent.height);
                if(x1<=x0||y1<=y0)continue;
                VkClearAttachment attachment{};
                attachment.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
                attachment.colorAttachment=0;
                attachment.clearValue.color.float32[0]=rect.Color[0];
                attachment.clearValue.color.float32[1]=rect.Color[1];
                attachment.clearValue.color.float32[2]=rect.Color[2];
                attachment.clearValue.color.float32[3]=1.f;
                VkClearRect area{};
                area.rect.offset={static_cast<std::int32_t>(x0),static_cast<std::int32_t>(y0)};
                area.rect.extent={static_cast<std::uint32_t>(x1-x0),static_cast<std::uint32_t>(y1-y0)};
                area.layerCount=1;
                vkCmdClearAttachments(command.Handle(),1,&attachment,1,&area);
            }
            vkCmdEndRenderPass(command.Handle());

            // Object-ID attachment is R32Uint, not the HDR scene color.
            // The viewport render pass supplies transfer-source layout for
            // a single pixel; caller reads mapped VkBuffer next frame ONLY
            // after VulkanRenderer::Retire waits for the completion fence.
            if (pickDestination != VK_NULL_HANDLE && pickPixel.has_value())
            {
                VkBufferImageCopy copy{};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                copy.imageOffset = { pickPixel->x, pickPixel->y, 0 };
                copy.imageExtent = { 1u, 1u, 1u };
                vkCmdCopyImageToBuffer(command.Handle(), m_Viewport.ObjectIDImage(),
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pickDestination, 1u, &copy);
            }
            // The offscreen viewport color is ordinarily shader-readable.
            // Explicitly transition it to TRANSFER_SRC_OPTIMAL, copy the full
            // RGBA16F image to a host-visible buffer, and transition it back
            // for editor sampling. The readback consumer waits on the fence.
            if (captureDestination != VK_NULL_HANDLE)
            {
                VkImageMemoryBarrier toTransfer{};
                toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
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
            else
            {
                vkCmdBindPipeline(command.Handle(),VK_PIPELINE_BIND_POINT_GRAPHICS,m_PresentPipeline);
                vkCmdBindDescriptorSets(command.Handle(),VK_PIPELINE_BIND_POINT_GRAPHICS,m_PresentLayout,
                    0u,1u,&m_PresentSet,0u,nullptr);
                vkCmdDraw(command.Handle(),3u,1u,0u,0u);
            }
            vkCmdEndRenderPass(command.Handle());
            command.End();
        }

    private:
        /// std140-compatible view/projection plus directional-light data.
        struct CameraUniform final { std::array<float, 336> Values{}; };
        /// 64-byte model + three padded normal columns + 16-byte tint exactly
        /// fit Vulkan's guaranteed minimum 128-byte push-constant capacity.
        struct MeshPushConstants final { std::array<float, 32> Values{}; };
        // Vertex binding 2, instance rate. Keep integer identity separate
        // from floating point attributes (all 32 object-ID bits survive).
        struct InstanceVertex final
        {
            std::array<float, 28> Matrices{};
            std::array<std::uint32_t, 2> Flags{};
        };
        static_assert(offsetof(InstanceVertex, Flags) == 112u);
        static_assert(sizeof(InstanceVertex) == 120u);
        struct GpuMesh final
        {
            std::unique_ptr<VulkanHostBuffer> Vertices;
            std::unique_ptr<VulkanHostBuffer> Skin;
            std::unique_ptr<VulkanHostBuffer> Indices;
            std::uint32_t IndexCount = 0u;
            std::size_t RequiredJoints = 0u;
            detail::LocalBounds Bounds;
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
        VkDescriptorSetLayout m_PresentSetLayout=VK_NULL_HANDLE;
        VkDescriptorPool m_PresentPool=VK_NULL_HANDLE;
        VkDescriptorSet m_PresentSet=VK_NULL_HANDLE;
        VkPipelineLayout m_PresentLayout=VK_NULL_HANDLE;
        VkPipeline m_PresentPipeline=VK_NULL_HANDLE;
        std::uint64_t m_PresentGeneration=0;
        VkPipeline m_MeshPipeline = VK_NULL_HANDLE;
        VkPipeline m_InstancedMeshPipeline = VK_NULL_HANDLE;
        VkPipeline m_InstancedShadowPipeline = VK_NULL_HANDLE;
        VkPipeline m_SkinnedMeshPipeline = VK_NULL_HANDLE;
        VkPipeline m_TransparentPipeline = VK_NULL_HANDLE;
        VkPipeline m_SkinnedTransparentPipeline = VK_NULL_HANDLE;
        VkPipeline m_DebugLinePipeline = VK_NULL_HANDLE;
        VkPipeline m_ShadowPipeline = VK_NULL_HANDLE;
        VkPipeline m_SkinnedShadowPipeline = VK_NULL_HANDLE;
        VulkanHostBuffer m_UniformBuffer;
        VulkanDirectionalShadowMap m_ShadowMap;
        std::unique_ptr<VulkanTexture2D> m_FallbackWhite;
        std::unique_ptr<VulkanTexture2D> m_FallbackNormal;
        std::unique_ptr<VulkanTexture2D> m_FallbackBlack;
        VulkanMaterialDescriptors m_MaterialDescriptors;
        VulkanViewportTarget m_Viewport;
        std::vector<ViewportOverlayRect> m_ViewportOverlay;
        ShowcaseCamera m_Camera;
        DirectionalShadowSettings m_ShadowSettings;
        ViewportShadingMode m_ShadingMode = ViewportShadingMode::Lit;
        std::vector<VkFramebuffer> m_Framebuffers;
        std::unordered_map<MeshHandle, GpuMesh> m_Meshes;
        detail::FenceRetiredResources<GpuMesh> m_RetiredMeshes{256u};
        std::unordered_map<TextureHandle, std::unique_ptr<VulkanTexture2D>> m_Textures;
        std::vector<MeshDraw> m_Draws;
        std::vector<detail::InstanceRun> m_InstanceRuns;
        std::vector<std::size_t> m_MainIndices, m_ShadowIndices;
        std::vector<detail::InstanceRun> m_ShadowInstanceRuns;
        VkDeviceSize m_ShadowInstanceOffset = 0u;
        std::vector<InstanceVertex> m_InstanceScratch;
        std::vector<std::uint8_t> m_Visible;
        detail::StaticVisibilityBvh m_VisibilityTree;
        std::vector<std::size_t> m_VisibilityFallback;
        bool m_VisibilityWarm = false, m_VisibilityTreeValid = false;
        std::unique_ptr<VulkanHostBuffer> m_InstanceBuffer;
        VkDeviceSize m_InstanceCapacity = 0u;
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
            try { CreateRenderPass(swapchain.Format()); CreateFramebuffers(swapchain); CreatePipelines(); CreatePresentation(swapchain.Format()); }
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

        // A standalone game has no editor overlay callback. Sample the
        // actual HDR viewport into its swapchain instead of presenting only
        // the clear color. The editor still owns its custom callback path.
        void CreatePresentation(VkFormat format)
        {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo descriptor{};
            descriptor.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptor.bindingCount=1;descriptor.pBindings=&binding;
            if(vkCreateDescriptorSetLayout(m_Device,&descriptor,nullptr,&m_PresentSetLayout)!=VK_SUCCESS)
                throw std::runtime_error("Vulkan presentation descriptor layout failed.");
            VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
            VkDescriptorPoolCreateInfo pool{};
            pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&poolSize;
            if(vkCreateDescriptorPool(m_Device,&pool,nullptr,&m_PresentPool)!=VK_SUCCESS)
                throw std::runtime_error("Vulkan presentation descriptor pool failed.");
            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool=m_PresentPool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&m_PresentSetLayout;
            if(vkAllocateDescriptorSets(m_Device,&allocate,&m_PresentSet)!=VK_SUCCESS)
                throw std::runtime_error("Vulkan presentation descriptor allocation failed.");
            VkPipelineLayoutCreateInfo layout{};
            layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout.setLayoutCount=1;layout.pSetLayouts=&m_PresentSetLayout;
            if(vkCreatePipelineLayout(m_Device,&layout,nullptr,&m_PresentLayout)!=VK_SUCCESS)
                throw std::runtime_error("Vulkan presentation pipeline layout failed.");
            VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
            try
            {
                vertex=CreateShaderModule(ReadSpirv("viewport_present.vert.spv"));
                fragment=CreateShaderModule(ReadSpirv("viewport_present.frag.spv"));
                // SRGB attachments encode linear shader output automatically.
                // Common UNORM swapchains need explicit display encoding.
                const std::uint32_t manualGamma=(format==VK_FORMAT_B8G8R8A8_UNORM||
                    format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_A8B8G8R8_UNORM_PACK32)?1u:0u;
                const VkSpecializationMapEntry entry{0,0,sizeof(manualGamma)};
                const VkSpecializationInfo specialization{1,&entry,sizeof(manualGamma),&manualGamma};
                std::array<VkPipelineShaderStageCreateInfo,2> stages{};
                stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[0].pName="main";
                stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;stages[1].pName="main";
                stages[1].pSpecializationInfo=&specialization;
                VkPipelineVertexInputStateCreateInfo input{};
                input.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                VkPipelineInputAssemblyStateCreateInfo assembly{};
                assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo viewport{};
                viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                viewport.viewportCount=1;viewport.scissorCount=1;
                VkPipelineRasterizationStateCreateInfo raster{};
                raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;
                raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1.f;
                VkPipelineMultisampleStateCreateInfo samples{};
                samples.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
                VkPipelineColorBlendAttachmentState color{};
                color.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                    VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
                VkPipelineColorBlendStateCreateInfo blend{};
                blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                blend.attachmentCount=1;blend.pAttachments=&color;
                const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
                VkPipelineDynamicStateCreateInfo dynamic{};
                dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamic.dynamicStateCount=static_cast<std::uint32_t>(dynamicStates.size());
                dynamic.pDynamicStates=dynamicStates.data();
                VkGraphicsPipelineCreateInfo create{};
                create.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                create.stageCount=static_cast<std::uint32_t>(stages.size());create.pStages=stages.data();
                create.pVertexInputState=&input;create.pInputAssemblyState=&assembly;
                create.pViewportState=&viewport;create.pRasterizationState=&raster;
                create.pMultisampleState=&samples;create.pColorBlendState=&blend;create.pDynamicState=&dynamic;
                create.layout=m_PresentLayout;create.renderPass=m_RenderPass;
                if(vkCreateGraphicsPipelines(m_Device,VK_NULL_HANDLE,1,&create,nullptr,&m_PresentPipeline)!=VK_SUCCESS)
                    throw std::runtime_error("Vulkan viewport presentation pipeline failed.");
            }
            catch(...)
            {
                if(fragment)vkDestroyShaderModule(m_Device,fragment,nullptr);
                if(vertex)vkDestroyShaderModule(m_Device,vertex,nullptr);
                throw;
            }
            vkDestroyShaderModule(m_Device,fragment,nullptr);
            vkDestroyShaderModule(m_Device,vertex,nullptr);
        }
        // Record runs after the frame fence. A viewport resize destroys old
        // image views only at device idle; bind the new generation before use.
        void RefreshPresentationDescriptor()
        {
            const auto texture=m_Viewport.Texture();
            if(!texture.IsValid())throw std::runtime_error("Invalid Vulkan presentation viewport.");
            if(texture.Generation==m_PresentGeneration)return;
            const VkDescriptorImageInfo image{texture.Sampler,texture.View,texture.Layout};
            VkWriteDescriptorSet write{};
            write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet=m_PresentSet;write.dstBinding=0;write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
            vkUpdateDescriptorSets(m_Device,1,&write,0,nullptr);
            m_PresentGeneration=texture.Generation;
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
            m_ShadowPipeline = CreateShadowPipeline("shadow.vert.spv", meshInput);

            // Vulkan's minimum limits cover 12 attributes, three binding
            // slots and this 120-byte stride. Binding 1 remains for skinning;
            // binding 2 is independent and cannot disturb that stream.
            const VkVertexInputBindingDescription instanceBinding{
                2u, static_cast<std::uint32_t>(sizeof(InstanceVertex)), VK_VERTEX_INPUT_RATE_INSTANCE };
            const std::array instanceBindings{ meshBinding, instanceBinding };
            std::array<VkVertexInputAttributeDescription, 12u> instanceAttributes{};
            std::copy(meshAttributes.begin(), meshAttributes.end(), instanceAttributes.begin());
            for (std::uint32_t column = 0u; column < 7u; ++column)
                instanceAttributes[4u + column] = { 4u + column, 2u,
                    VK_FORMAT_R32G32B32A32_SFLOAT, column * 16u };
            instanceAttributes[11] = { 11u, 2u, VK_FORMAT_R32G32_UINT,
                static_cast<std::uint32_t>(offsetof(InstanceVertex, Flags)) };
            auto instanceInput = meshInput;
            instanceInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(instanceBindings.size());
            instanceInput.pVertexBindingDescriptions = instanceBindings.data();
            instanceInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(instanceAttributes.size());
            instanceInput.pVertexAttributeDescriptions = instanceAttributes.data();
            m_InstancedMeshPipeline = CreatePipeline("instanced_triangle.vert.spv",
                "instanced_triangle.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                instanceInput, VK_TRUE, false);
            const std::array shadowInstanceAttributes{
                instanceAttributes[0], instanceAttributes[4], instanceAttributes[5],
                instanceAttributes[6], instanceAttributes[7] };
            auto shadowInstanceInput = instanceInput;
            shadowInstanceInput.vertexAttributeDescriptionCount =
                static_cast<std::uint32_t>(shadowInstanceAttributes.size());
            shadowInstanceInput.pVertexAttributeDescriptions = shadowInstanceAttributes.data();
            m_InstancedShadowPipeline = CreateShadowPipeline(
                "instanced_shadow.vert.spv", shadowInstanceInput);

            VkVertexInputBindingDescription skinBinding{};
            skinBinding.binding = 1u;
            skinBinding.stride = sizeof(SkinVertexInfluence);
            skinBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            const std::array skinnedBindings{ meshBinding, skinBinding };
            const std::array skinnedAttributes{
                meshAttributes[0], meshAttributes[1], meshAttributes[2], meshAttributes[3],
                VkVertexInputAttributeDescription{ 4u, 1u, VK_FORMAT_R32G32B32A32_UINT,
                    offsetof(SkinVertexInfluence, Joints) },
                VkVertexInputAttributeDescription{ 5u, 1u, VK_FORMAT_R32G32B32A32_SFLOAT,
                    offsetof(SkinVertexInfluence, Weights) }
            };
            VkPipelineVertexInputStateCreateInfo skinnedInput{};
            skinnedInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            skinnedInput.vertexBindingDescriptionCount =
                static_cast<std::uint32_t>(skinnedBindings.size());
            skinnedInput.pVertexBindingDescriptions = skinnedBindings.data();
            skinnedInput.vertexAttributeDescriptionCount =
                static_cast<std::uint32_t>(skinnedAttributes.size());
            skinnedInput.pVertexAttributeDescriptions = skinnedAttributes.data();
            m_SkinnedMeshPipeline = CreatePipeline("skinned_triangle.vert.spv",
                "triangle.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                skinnedInput, VK_TRUE, false);
            m_SkinnedTransparentPipeline = CreatePipeline("skinned_triangle.vert.spv",
                "triangle.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                skinnedInput, VK_FALSE, true);
            m_SkinnedShadowPipeline = CreateShadowPipeline(
                "skinned_shadow.vert.spv", skinnedInput);

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
        [[nodiscard]] VkPipeline CreateShadowPipeline(const std::string& vertexName,
            const VkPipelineVertexInputStateCreateInfo& vertexInput) const
        {
            const VkShaderModule vertex = CreateShaderModule(ReadSpirv(vertexName));
            try
            {
                VkPipelineShaderStageCreateInfo stage{};
                stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
                stage.module = vertex;
                stage.pName = "main";
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
                rasterizer.depthBiasEnable = VK_TRUE;
                rasterizer.lineWidth = 1.0f;
                VkPipelineMultisampleStateCreateInfo multisampling{};
                multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo depth{};
                depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                depth.depthTestEnable = VK_TRUE;
                depth.depthWriteEnable = VK_TRUE;
                depth.depthCompareOp = VK_COMPARE_OP_LESS;
                VkPipelineColorBlendStateCreateInfo blend{};
                blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                const std::array dynamicStates{
                    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS
                };
                VkPipelineDynamicStateCreateInfo dynamic{};
                dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
                dynamic.pDynamicStates = dynamicStates.data();
                VkGraphicsPipelineCreateInfo create{};
                create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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
            VkPipelineShaderStageCreateInfo vertexStage{};
            vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT; vertexStage.module = vertex; vertexStage.pName = "main";
            VkPipelineShaderStageCreateInfo fragmentStage{};
            fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT; fragmentStage.module = fragment; fragmentStage.pName = "main";
            const std::array stages{ vertexStage, fragmentStage };
            VkPipelineInputAssemblyStateCreateInfo assembly{};
            assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = topology;
            VkPipelineViewportStateCreateInfo viewport{};
            viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport.viewportCount = 1u; viewport.scissorCount = 1u;
            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL; rasterizer.cullMode = VK_CULL_MODE_NONE; rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rasterizer.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{};
            depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
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
            VkPipelineColorBlendStateCreateInfo blend{};
            blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            const std::array blendAttachments{ attachment, objectAttachment };
            blend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
            blend.pAttachments = blendAttachments.data();
            const std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()); dynamic.pDynamicStates = dynamicStates.data();
            VkGraphicsPipelineCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            create.stageCount = static_cast<std::uint32_t>(stages.size()); create.pStages = stages.data(); create.pVertexInputState = &vertexInput;
            create.pInputAssemblyState = &assembly; create.pViewportState = &viewport; create.pRasterizationState = &rasterizer;
            create.pMultisampleState = &multisampling; create.pDepthStencilState = &depth; create.pColorBlendState = &blend;
            create.pDynamicState = &dynamic; create.layout = m_Layout; create.renderPass = m_Viewport.RenderPass();
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1u, &create, nullptr, &pipeline) != VK_SUCCESS) throw std::runtime_error("vkCreateGraphicsPipelines failed.");
            return pipeline;
        }

        // Bind the set associated with MeshDraw index: camera, shadow map,
        // base/normal/metallic-roughness/emissive/occlusion, material factors,
        // environment and optional skin palette. This does not create a new
        // set; RebuildMaterialDescriptors already populated each binding.
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
            VkClearValue clear{};
            clear.depthStencil = VkClearDepthStencilValue{ 1.0f, 0u };
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
            constexpr VkDeviceSize offsets[2]{ 0u, 0u };
            detail::DrawBindingState<VkPipeline, MeshHandle> state;
            VkDeviceSize instanceOffset = m_ShadowInstanceOffset;
            for (const auto& run : m_ShadowInstanceRuns)
            {
                const auto drawIndex = m_ShadowIndices[run.First];
                const MeshDraw& draw = m_Draws[drawIndex];
                if (!draw.CastShadows ||
                    draw.Material.AlphaMode == MaterialAlphaMode::Blend) continue;
                const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
                BindMaterial(command, drawIndex);
                if (run.Count > 1u)
                {
                    state.PipelineFor(m_InstancedShadowPipeline, [&]
                    {
                        vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_InstancedShadowPipeline);
                    });
                    state.MeshFor(draw.Mesh, [&]
                    {
                        const VkBuffer vertex = mesh.Vertices->Handle();
                        vkCmdBindVertexBuffers(command.Handle(), 0u, 1u, &vertex, offsets);
                        vkCmdBindIndexBuffer(command.Handle(), mesh.Indices->Handle(), 0u,
                            VK_INDEX_TYPE_UINT32);
                    });
                    const VkBuffer buffer = m_InstanceBuffer->Handle();
                    vkCmdBindVertexBuffers(command.Handle(), 2u, 1u, &buffer, &instanceOffset);
                    vkCmdDrawIndexed(command.Handle(), mesh.IndexCount, run.Count, 0u, 0, 0u);
                    instanceOffset += static_cast<VkDeviceSize>(run.Count) * sizeof(InstanceVertex);
                    continue;
                }
                const VkPipeline pipeline = mesh.RequiredJoints > 0u
                    ? m_SkinnedShadowPipeline : m_ShadowPipeline;
                state.PipelineFor(pipeline, [&]
                {
                    vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                });
                MeshPushConstants push{};
                CopyTranspose(draw.Model, push.Values.data());
                // Vulkan requires every stage declared by an overlapping
                // pipeline-layout range, even when this pipeline has no
                // fragment shader consuming the bytes.
                vkCmdPushConstants(command.Handle(), m_Layout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0u, sizeof(push), &push);
                state.MeshFor(draw.Mesh, [&]
                {
                    const VkBuffer buffers[2]{ mesh.Vertices->Handle(),
                        mesh.Skin ? mesh.Skin->Handle() : VK_NULL_HANDLE };
                    const std::uint32_t bufferCount = mesh.Skin ? 2u : 1u;
                    vkCmdBindVertexBuffers(command.Handle(), 0u, bufferCount, buffers, offsets);
                    vkCmdBindIndexBuffer(command.Handle(), mesh.Indices->Handle(), 0u,
                        VK_INDEX_TYPE_UINT32);
                });
                vkCmdDrawIndexed(command.Handle(), mesh.IndexCount, 1u, 0u, 0, 0u);
            }
            vkCmdEndRenderPass(command.Handle());
        }

        // Camera visibility is separate from shadow visibility: an object
        // outside the camera may still cast a shadow into the viewport.
        // Rest-pose bounds cannot enclose arbitrary skeletal deformation.
        void InvalidateVisibilityTree() noexcept
        {
            m_VisibilityWarm = false;
            m_VisibilityTreeValid = false;
        }

        [[nodiscard]] bool SameVisibilityGeometry(const std::vector<MeshDraw>& draws) const noexcept
        {
            if (draws.size() != m_Draws.size()) return false;
            for (std::size_t i = 0; i < draws.size(); ++i)
            {
                if (draws[i].Mesh != m_Draws[i].Mesh) return false;
                for (std::size_t r = 0; r < 4; ++r)
                    for (std::size_t c = 0; c < 4; ++c)
                        if (!detail::SameDescriptorFloat(draws[i].Model(r,c), m_Draws[i].Model(r,c)))
                            return false;
            }
            return true;
        }

        void BuildVisibilityTree()
        {
            std::vector<detail::StaticVisibilityBvh::Entry> entries;
            m_VisibilityFallback.clear();
            for (std::size_t i = 0; i < m_Draws.size(); ++i)
            {
                const auto& draw = m_Draws[i];
                const auto& mesh = m_Meshes.at(draw.Mesh);
                const auto bounds = mesh.RequiredJoints > 0u ? detail::LocalBounds{} :
                    detail::ConservativeWorldBounds(mesh.Bounds, draw.Model);
                if (bounds.Valid) entries.push_back({i,bounds});
                else m_VisibilityFallback.push_back(i);
            }
            m_VisibilityTree.Build(std::move(entries));
            m_VisibilityTreeValid = true;
        }

        void PrepareVisibility()
        {
            const auto view = m_Camera.View();
            const auto extent = m_Viewport.Extent();
            const auto projection = m_Camera.Projection(extent.width, extent.height);
            const detail::PreparedVulkanFrustum frustum(view, projection);
            m_Visible.resize(m_Draws.size());
            const auto test = [&](std::size_t index)
            {
                const auto& draw = m_Draws[index];
                const auto& mesh = m_Meshes.at(draw.Mesh);
                m_Visible[index] = static_cast<std::uint8_t>(mesh.RequiredJoints > 0u ||
                    !frustum.Outside(mesh.Bounds, draw.Model));
            };

            // Never rebuild a spatial tree every frame in a moving scene.
            // Wait for a repeated geometry submission/frame; material and
            // camera changes do not invalidate the world-space hierarchy.
            constexpr std::size_t minimumTreeDraws = 128u;
            if (m_Draws.size() >= minimumTreeDraws && m_VisibilityWarm)
            {
                if (!m_VisibilityTreeValid) BuildVisibilityTree();
                const auto identity = kairo::foundation::math::Mat4f::Identity();
                m_VisibilityTree.Cull(
                    [&](const detail::LocalBounds& box) { return frustum.Outside(box,identity); },
                    [&](std::size_t index) { m_Visible[index] = 0u; }, test);
                for (const auto index : m_VisibilityFallback) test(index);
            }
            else
                for (std::size_t index = 0; index < m_Draws.size(); ++index) test(index);
            m_VisibilityWarm = true;
        }

        // Pack independent main and shadow ranges into one fence-retired buffer.
        void PrepareInstances()
        {
            detail::PlanIndexedInstanceRuns(m_Draws,
                [this](std::size_t index, const MeshDraw& draw)
                {
                    return m_Visible[index] != 0u && draw.Material.AlphaMode != MaterialAlphaMode::Blend;
                },
                [](const MeshDraw& draw)
                {
                    return draw.Material.AlphaMode == MaterialAlphaMode::Opaque && draw.Skinning.Empty();
                },
                [](const MeshDraw& a, const MeshDraw& b)
                {
                    return a.Mesh == b.Mesh && detail::SameDescriptorMaterial(a.Material, b.Material);
                }, m_MainIndices, m_InstanceRuns);
            auto& instances = m_InstanceScratch;
            instances.clear();
            for (const auto& run : m_InstanceRuns)
            {
                if (run.Count < 2u) continue;
                for (std::size_t i = 0u; i < run.Count; ++i)
                {
                    const auto& draw = m_Draws[m_MainIndices[run.First + i]];
                    InstanceVertex vertex{};
                    CopyTranspose(draw.Model, vertex.Matrices.data());
                    const auto normal = ComputeNormalMatrix(draw.Model);
                    for (std::size_t column = 0u; column < 3u; ++column)
                        for (std::size_t row = 0u; row < 3u; ++row)
                            vertex.Matrices[16u + column * 4u + row] = normal(row, column);
                    vertex.Flags = { draw.ObjectID, draw.ReceiveShadows ? 1u : 0u };
                    instances.push_back(vertex);
                }
            }
            if (instances.size() > std::numeric_limits<VkDeviceSize>::max() / sizeof(InstanceVertex))
                throw std::length_error("Vulkan main instance range exceeds buffer size range.");
            m_ShadowInstanceOffset = static_cast<VkDeviceSize>(instances.size()) * sizeof(InstanceVertex);
            // Deliberately independent of m_Visible. Camera-offscreen objects
            // remain eligible shadow casters; no camera bounds on the light pass.
            detail::PlanIndexedInstanceRuns(m_Draws,
                [](std::size_t, const MeshDraw& draw)
                {
                    return draw.CastShadows && draw.Material.AlphaMode != MaterialAlphaMode::Blend;
                },
                [](const MeshDraw& draw)
                {
                    return draw.Skinning.Empty() && draw.Material.AlphaMode == MaterialAlphaMode::Opaque;
                },
                [](const MeshDraw& a, const MeshDraw& b)
                {
                    return a.Mesh == b.Mesh && detail::SameDescriptorMaterial(a.Material, b.Material);
                }, m_ShadowIndices, m_ShadowInstanceRuns);
            for (const auto& run : m_ShadowInstanceRuns)
            {
                if (run.Count < 2u) continue;
                for (std::size_t i = 0u; i < run.Count; ++i)
                {
                    InstanceVertex vertex{};
                    CopyTranspose(m_Draws[m_ShadowIndices[run.First + i]].Model, vertex.Matrices.data());
                    instances.push_back(vertex); // depth shader consumes model columns only
                }
            }
            if (instances.empty()) return;
            if (instances.size() > std::numeric_limits<VkDeviceSize>::max() / sizeof(InstanceVertex))
                throw std::length_error("Vulkan instance upload exceeds buffer size range.");
            const auto bytes = static_cast<VkDeviceSize>(instances.size()) * sizeof(InstanceVertex);
            if (bytes > m_InstanceCapacity)
            {
                // Allocate before publishing; old storage is fence-retired.
                auto capacity = bytes;
                if (m_InstanceCapacity <= std::numeric_limits<VkDeviceSize>::max() / 2u)
                    capacity = std::max(bytes, m_InstanceCapacity * 2u);
                auto buffer = std::make_unique<VulkanHostBuffer>(m_VulkanDevice, capacity,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                m_InstanceBuffer = std::move(buffer);
                m_InstanceCapacity = capacity;
            }
            m_InstanceBuffer->Write(instances.data(), bytes);
        }

        void DrawMeshes(const VulkanCommandBuffer& command) const
        {
            if (m_Draws.empty()) return;
            constexpr VkDeviceSize offsets[2]{ 0u, 0u };
            // Fresh state for this pass; opaque and sorted transparent draws
            // share it without changing either submission order.
            detail::DrawBindingState<VkPipeline, MeshHandle> state;
            const auto drawOne = [&](std::size_t drawIndex, bool transparent)
            {
                const MeshDraw& draw = m_Draws[drawIndex];
                BindMaterial(command, drawIndex);
                const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
                const bool skinned = mesh.RequiredJoints > 0u;
                const VkPipeline pipeline = transparent
                    ? (skinned ? m_SkinnedTransparentPipeline : m_TransparentPipeline)
                    : (skinned ? m_SkinnedMeshPipeline : m_MeshPipeline);
                state.PipelineFor(pipeline, [&]
                {
                    vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                });
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
                state.MeshFor(draw.Mesh, [&]
                {
                    const VkBuffer buffers[2]{ mesh.Vertices->Handle(),
                        mesh.Skin ? mesh.Skin->Handle() : VK_NULL_HANDLE };
                    const std::uint32_t bufferCount = mesh.Skin ? 2u : 1u;
                    vkCmdBindVertexBuffers(command.Handle(), 0u, bufferCount, buffers, offsets);
                    vkCmdBindIndexBuffer(command.Handle(), mesh.Indices->Handle(), 0u,
                        VK_INDEX_TYPE_UINT32);
                });
                vkCmdDrawIndexed(command.Handle(), mesh.IndexCount, 1u, 0u, 0, 0u);
            };

            VkDeviceSize instanceOffset = 0u;
            for (const auto& run : m_InstanceRuns)
            {
                const auto drawIndex = m_MainIndices[run.First];
                const auto& draw = m_Draws[drawIndex];
                if (run.Count == 1u)
                {
                    drawOne(drawIndex, false);
                    continue;
                }
                const auto& mesh = m_Meshes.at(draw.Mesh);
                BindMaterial(command, drawIndex);
                state.PipelineFor(m_InstancedMeshPipeline, [&]
                {
                    vkCmdBindPipeline(command.Handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, m_InstancedMeshPipeline);
                });
                state.MeshFor(draw.Mesh, [&]
                {
                    const VkBuffer vertex = mesh.Vertices->Handle();
                    vkCmdBindVertexBuffers(command.Handle(), 0u, 1u, &vertex, offsets);
                    vkCmdBindIndexBuffer(command.Handle(), mesh.Indices->Handle(), 0u, VK_INDEX_TYPE_UINT32);
                });
                // Bind the exact packed run; instanceCount drives the GPU
                // fetch rate. Individual IDs and matrices come from binding 2.
                const VkBuffer instanceBuffer = m_InstanceBuffer->Handle();
                vkCmdBindVertexBuffers(command.Handle(), 2u, 1u, &instanceBuffer, &instanceOffset);
                vkCmdDrawIndexed(command.Handle(), mesh.IndexCount, run.Count, 0u, 0, 0u);
                instanceOffset += static_cast<VkDeviceSize>(run.Count) * sizeof(InstanceVertex);
            }

            std::vector<std::size_t> transparentDraws;
            transparentDraws.reserve(m_Draws.size());
            for (std::size_t index = 0u; index < m_Draws.size(); ++index)
                if (m_Visible[index] != 0u && m_Draws[index].Material.AlphaMode == MaterialAlphaMode::Blend)
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
            for (const std::size_t index : transparentDraws) drawOne(index, true);
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

        // Missing optional texture -> purpose-selected fallback texture.
        // Explicit nonzero unknown handles throw in map::at. Never treat an
        // authored missing reference as a silently valid fallback handle.
        [[nodiscard]] const VulkanTexture2D& TextureOr(
            TextureHandle handle, const VulkanTexture2D& fallback) const
        {
            if (handle == InvalidTextureHandle) return fallback;
            return *m_Textures.at(handle);
        }

        // On scene/texture/palette changes, resolve every submitted draw's
        // five texture handles into native VkImageView+VkSampler pairs; use
        // owned 1x1 fallback textures for optional channels. Pass borrowed
        // views and source palette pointers into VulkanMaterialDescriptors,
        // which COPIES palette bytes to GPU-visible UBO storage. Set must be
        // rebuilt only after old GPU submissions no longer reference it.
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
                binding.Skinning = draw.Skinning.Empty() ? nullptr : &draw.Skinning;
                for (std::size_t channel = 0u; channel < textures.size(); ++channel)
                {
                    binding.Views[channel] = textures[channel]->View();
                    binding.Samplers[channel] = textures[channel]->Sampler();
                }
                const VulkanTexture2D& environment = TextureOr(
                    m_Environment.EnvironmentTexture, *m_FallbackBlack);
                binding.EnvironmentView = environment.View();
                binding.EnvironmentSampler = environment.Sampler();
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
            // viewportParameters.zw retain density/start. sceneParameters.w
            // holds the equation ID; one vec4 follows the unchanged light arrays.
            uniform.Values[66u] = m_Environment.FogDensity;
            uniform.Values[67u] = m_Environment.FogStartDistance;
            uniform.Values[68u] = static_cast<float>(m_Lights.size());
            uniform.Values[69u] = shadowIndex.has_value()
                ? static_cast<float>(*shadowIndex) : -1.0f;
            uniform.Values[70u] = m_Environment.EnvironmentIntensity;
            uniform.Values[71u] = static_cast<float>(m_Environment.EffectiveFogMode());
            const auto fogColor=m_Environment.EffectiveFogColor();
            uniform.Values[332u]=fogColor.x;
            uniform.Values[333u]=fogColor.y;
            uniform.Values[334u]=fogColor.z;
            uniform.Values[335u]=m_Environment.FogEndDistance;
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

        [[nodiscard]] VkShaderModule CreateShaderModule(std::span<const std::uint32_t> spirv) const
        {
            VkShaderModuleCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            create.codeSize = spirv.size() * sizeof(std::uint32_t); create.pCode = spirv.data(); VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(m_Device, &create, nullptr, &module) != VK_SUCCESS) throw std::runtime_error("vkCreateShaderModule failed.");
            return module;
        }

        // Embedded immutable words: no runtime build-directory dependency.
        [[nodiscard]] static std::span<const std::uint32_t> ReadSpirv(const std::string& fileName)
        {
            return detail::EmbeddedVulkanShader(fileName);
        }

        // Release swapchain-dependent framebuffers, graphics pipelines,
        // layout and render pass. Texture/mesh maps are kept across resize;
        // their ultimate destruction comes from this object's member RAII.
        // VulkanRenderer guarantees device idle before calling release.
        void Destroy() noexcept
        {
            m_DebugVertexBuffer.reset(); m_DebugVertexCapacity = 0u;
            m_InstanceBuffer.reset(); m_InstanceCapacity = 0u;
            for (const VkFramebuffer framebuffer : m_Framebuffers) vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
            m_Framebuffers.clear();
            if(m_PresentPipeline)vkDestroyPipeline(m_Device,m_PresentPipeline,nullptr);
            if(m_PresentLayout)vkDestroyPipelineLayout(m_Device,m_PresentLayout,nullptr);
            if(m_PresentPool)vkDestroyDescriptorPool(m_Device,m_PresentPool,nullptr);
            if(m_PresentSetLayout)vkDestroyDescriptorSetLayout(m_Device,m_PresentSetLayout,nullptr);
            m_PresentPipeline=VK_NULL_HANDLE;m_PresentLayout=VK_NULL_HANDLE;
            m_PresentPool=VK_NULL_HANDLE;m_PresentSetLayout=VK_NULL_HANDLE;
            m_PresentSet=VK_NULL_HANDLE;m_PresentGeneration=0;
            if (m_SkinnedShadowPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_Device, m_SkinnedShadowPipeline, nullptr);
            if (m_ShadowPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
            if (m_DebugLinePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_DebugLinePipeline, nullptr);
            if (m_SkinnedTransparentPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_Device, m_SkinnedTransparentPipeline, nullptr);
            if (m_TransparentPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_Device, m_TransparentPipeline, nullptr);
            if (m_SkinnedMeshPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_Device, m_SkinnedMeshPipeline, nullptr);
            if (m_InstancedShadowPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_InstancedShadowPipeline, nullptr);
            if (m_InstancedMeshPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_InstancedMeshPipeline, nullptr);
            if (m_MeshPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_Device, m_MeshPipeline, nullptr);
            if (m_Layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_Device, m_Layout, nullptr);
            if (m_RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
            m_SkinnedShadowPipeline = VK_NULL_HANDLE; m_ShadowPipeline = VK_NULL_HANDLE;
            m_DebugLinePipeline = VK_NULL_HANDLE;
            m_SkinnedMeshPipeline = VK_NULL_HANDLE; m_MeshPipeline = VK_NULL_HANDLE;
            m_InstancedMeshPipeline = VK_NULL_HANDLE;
            m_InstancedShadowPipeline = VK_NULL_HANDLE;
            m_SkinnedTransparentPipeline = VK_NULL_HANDLE;
            m_TransparentPipeline = VK_NULL_HANDLE;
            m_Layout = VK_NULL_HANDLE; m_RenderPass = VK_NULL_HANDLE;
        }
    };
}
