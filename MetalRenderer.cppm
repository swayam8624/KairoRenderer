module;

#include "detail/MetalBackend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

export module Kairo.Renderer.MetalRuntime;

import Kairo.Foundation.Math;
import Kairo.Renderer.Types;
import Kairo.Renderer.RenderGraph;
import Kairo.Renderer.GraphicsBackend;
import Kairo.Renderer.Camera;
import Kairo.Renderer.Window;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.Texture;
import Kairo.Renderer.Material;
import Kairo.Renderer.RenderScene;
import Kairo.Renderer.ShadowSettings;
import Kairo.Renderer.DebugDraw;
import Kairo.Assets.TextureArtifact;

export namespace kairo::renderer
{
    /// Native Metal implementation of the renderer-neutral runtime API.
    ///
    /// Input: validated Kairo meshes, textures, scene state, camera, and debug
    /// lines. Output: an HDR offscreen viewport, object IDs, readback, and a
    /// CAMetalLayer presentation surface. Task: keep Objective-C ownership and
    /// Metal resource types behind `detail::MetalBackend` while preserving the
    /// same public behavior used by Vulkan and OpenGL hosts.
    class MetalRendererRuntime final
    {
        GlfwRuntime m_Glfw;
        Window m_Window;
        detail::MetalBackend m_Backend;
        ShowcaseCamera m_Camera;
        DirectionalShadowSettings m_ShadowSettings;
        ViewportShadingMode m_ShadingMode = ViewportShadingMode::Lit;
        std::unordered_set<MeshHandle> m_Meshes;
        std::unordered_set<TextureHandle> m_Textures;
        std::vector<MeshDraw> m_Draws;
        std::vector<RenderLight> m_Lights;
        RenderEnvironment m_Environment;
        std::vector<detail::MetalDebugVertex> m_DebugVertices;
        std::uint32_t m_ViewportWidth = 1u;
        std::uint32_t m_ViewportHeight = 1u;
        std::uint64_t m_ViewportGeneration = 1u;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> m_PickRequest;
        std::optional<std::uint32_t> m_PickResult;
        bool m_CaptureRequested = false;
        std::optional<ViewportCapture> m_CaptureResult;
        RenderGraphExecutionProfile m_LastFrameProfile;

    public:
        explicit MetalRendererRuntime(const WindowDesc& desc)
            : m_Glfw(), m_Window(desc, GraphicsBackend::Metal),
              m_Backend(m_Window.NativeHandle(), desc.Width, desc.Height),
              m_ViewportWidth(desc.Width), m_ViewportHeight(desc.Height)
        {
        }

        MetalRendererRuntime(const MetalRendererRuntime&) = delete;
        MetalRendererRuntime& operator=(const MetalRendererRuntime&) = delete;

        [[nodiscard]] GraphicsBackend Backend() const noexcept
        {
            return GraphicsBackend::Metal;
        }

        [[nodiscard]] Window& NativeWindow() noexcept { return m_Window; }

        /// Task: expose the native Metal render/present and readback
        /// boundaries as real graph callbacks. The detail backend still owns
        /// command encoding internally; the runtime graph owns when rendering,
        /// picking, and capture execute relative to one another.
        void DrawFrame()
        {
            const auto [windowWidth, windowHeight] = m_Window.FramebufferExtent();
            if (windowWidth == 0u || windowHeight == 0u)
            {
                m_LastFrameProfile = {};
                return;
            }
            m_Backend.SetDrawableSize(windowWidth, windowHeight);

            std::vector<detail::MetalDraw> draws;
            draws.reserve(m_Draws.size());
            for (const MeshDraw& draw : m_Draws) draws.push_back(Convert(draw));
            std::vector<detail::MetalLight> lights;
            lights.reserve(m_Lights.size());
            for (const RenderLight& light : m_Lights) lights.push_back(Convert(light));

            detail::MetalFrame frame;
            frame.Draws = draws;
            frame.Lights = lights;
            frame.DebugVertices = m_DebugVertices;
            Copy(m_Camera.View(), frame.View);
            auto projection =
                m_Camera.Projection(m_ViewportWidth, m_ViewportHeight);
            projection(1u, 1u) *= -1.0f;
            Copy(projection, frame.Projection);
            Copy(BuildLightViewProjection(), frame.LightViewProjection);
            const auto camera = m_Camera.Position();
            frame.CameraPosition[0] = camera.x;
            frame.CameraPosition[1] = camera.y;
            frame.CameraPosition[2] = camera.z;
            Copy(m_Environment.BackgroundColor, frame.Background);
            Copy(m_Environment.AmbientColor * m_Environment.AmbientIntensity,
                frame.Ambient);
            frame.Exposure = m_Environment.ExposureEV100;
            frame.EnvironmentIntensity = m_Environment.EnvironmentIntensity;
            frame.EnvironmentTexture = m_Environment.EnvironmentTexture;
            frame.ShadingMode = static_cast<std::uint32_t>(m_ShadingMode);
            frame.ShadowsEnabled = m_ShadowSettings.Enabled;
            frame.ShadowStrength = m_ShadowSettings.Strength;
            frame.ReceiverBias = m_ShadowSettings.ReceiverBias;
            frame.ConstantDepthBias = m_ShadowSettings.ConstantDepthBias;
            frame.SlopeDepthBias = m_ShadowSettings.SlopeDepthBias;

            const bool pickRequested = m_PickRequest.has_value();
            const bool captureRequested = m_CaptureRequested;

            RenderGraph graph;
            const auto viewport = graph.AddResource({ "MetalViewport",
                RenderResourceKind::External, 0u, false,
                RenderResourceState::ShaderRead });
            const auto nativeFrame = graph.AddResource({ "MetalNativeFrame",
                RenderResourceKind::External, 0u, false,
                RenderResourceState::Present });

            const auto renderPass = graph.AddPass("RenderPresent", {
                { viewport, RenderAccessMode::Write,
                    RenderResourceState::ColorAttachment },
                { nativeFrame, RenderAccessMode::ReadWrite,
                    RenderResourceState::Present }
            }, [this, &frame]
            {
                m_Backend.Draw(frame);
            });

            std::optional<RenderPassHandle> tail;
            if (pickRequested)
            {
                const auto pickPass = graph.AddPass("PickReadback", {
                    { viewport, RenderAccessMode::Read,
                        RenderResourceState::CopySource }
                }, [this]
                {
                    m_PickResult = m_Backend.Pick(
                        m_PickRequest->first, m_PickRequest->second);
                    m_PickRequest.reset();
                });
                graph.DependsOn(pickPass, renderPass);
                tail = pickPass;
            }

            if (captureRequested)
            {
                const auto capturePass = graph.AddPass("CaptureReadback", {
                    { viewport, RenderAccessMode::Read,
                        RenderResourceState::CopySource }
                }, [this]
                {
                    auto native = m_Backend.Capture();
                    ViewportCapture capture;
                    capture.Width = native.Width;
                    capture.Height = native.Height;
                    capture.RGBA.assign(native.RGBA.get(),
                        native.RGBA.get() + native.ByteCount);
                    m_CaptureResult = std::move(capture);
                    m_CaptureRequested = false;
                });
                graph.DependsOn(capturePass,
                    tail.has_value() ? *tail : renderPass);
            }

            m_LastFrameProfile = graph.Compile().Execute();
        }

        [[nodiscard]] const RenderGraphExecutionProfile& LastFrameProfile()
            const noexcept
        {
            return m_LastFrameProfile;
        }

        void SubmitDebugDraw(const DebugDrawList& debug)
        {
            if (debug.Lines().size() >
                std::numeric_limits<std::size_t>::max() / 2u)
                throw std::length_error("Metal debug-line count overflows storage.");
            m_DebugVertices.clear();
            m_DebugVertices.reserve(debug.Lines().size() * 2u);
            for (const DebugLine& line : debug.Lines())
            {
                m_DebugVertices.push_back(Convert(line.A, line.Color));
                m_DebugVertices.push_back(Convert(line.B, line.Color));
            }
        }

        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
            if (mesh.IsSkinned())
                throw std::logic_error(
                    "Skinned mesh upload requires the native GPU skinning path.");
            std::vector<detail::MetalVertex> vertices;
            vertices.reserve(mesh.Vertices().size());
            for (const MeshVertex& vertex : mesh.Vertices())
            {
                detail::MetalVertex converted{};
                Copy(vertex.Position, converted.Position);
                Copy(vertex.Color, converted.Color);
                Copy(vertex.Normal, converted.Normal);
                converted.TexCoord[0] = vertex.TexCoord.x;
                converted.TexCoord[1] = vertex.TexCoord.y;
                vertices.push_back(converted);
            }
            const MeshHandle handle = m_Backend.CreateMesh(
                vertices, mesh.Indices());
            m_Meshes.insert(handle);
            return handle;
        }

        void DestroyMesh(MeshHandle mesh)
        {
            if (!m_Meshes.contains(mesh))
                throw std::invalid_argument(
                    "Metal mesh handle is not owned by this runtime.");
            if (std::ranges::any_of(m_Draws,
                [&](const MeshDraw& draw) { return draw.Mesh == mesh; }))
                throw std::logic_error(
                    "Cannot destroy a Metal mesh used by the submitted scene.");
            m_Backend.DestroyMesh(mesh);
            m_Meshes.erase(mesh);
        }

        [[nodiscard]] TextureHandle CreateTexture(
            const kairo::assets::TextureArtifactData& texture,
            TextureSampler sampler = {})
        {
            kairo::assets::ValidateTextureArtifactData(texture);
            std::vector<std::byte> bytes;
            std::size_t total = 0u;
            for (const auto& mip : texture.Mips)
            {
                if (mip.Pixels.size() > std::numeric_limits<std::size_t>::max() - total)
                    throw std::length_error("Metal texture payload overflows storage.");
                total += mip.Pixels.size();
            }
            bytes.reserve(total);
            for (const auto& mip : texture.Mips)
                bytes.insert(bytes.end(), mip.Pixels.begin(), mip.Pixels.end());
            detail::MetalTextureUpload upload;
            upload.Width = texture.Mips.front().Width;
            upload.Height = texture.Mips.front().Height;
            upload.MipLevels = static_cast<std::uint32_t>(texture.Mips.size());
            upload.Format = Convert(texture.Format, texture.ColorSpace);
            upload.AddressU = Convert(sampler.AddressU);
            upload.AddressV = Convert(sampler.AddressV);
            upload.Bytes = bytes.data();
            upload.ByteCount = bytes.size();
            const TextureHandle handle = m_Backend.CreateTexture(upload);
            m_Textures.insert(handle);
            return handle;
        }

        void DestroyTexture(TextureHandle texture)
        {
            if (!m_Textures.contains(texture))
                throw std::invalid_argument(
                    "Metal texture handle is not owned by this runtime.");
            if (TextureIsSubmitted(texture))
                throw std::logic_error(
                    "Cannot destroy a Metal texture used by the submitted scene.");
            m_Backend.DestroyTexture(texture);
            m_Textures.erase(texture);
        }

        void SubmitRenderScene(const RenderScene& scene)
        {
            for (const MeshDraw& draw : scene.Draws())
            {
                RenderScene::Validate(draw);
                if (!m_Meshes.contains(draw.Mesh))
                    throw std::invalid_argument(
                        "RenderScene references a Metal mesh owned by another runtime.");
                ValidateTexture(draw.Material.BaseColorTexture);
                ValidateTexture(draw.Material.NormalTexture);
                ValidateTexture(draw.Material.MetallicRoughnessTexture);
                ValidateTexture(draw.Material.EmissiveTexture);
                ValidateTexture(draw.Material.OcclusionTexture);
            }
            ValidateTexture(scene.Environment().EnvironmentTexture);
            m_Draws = scene.Draws();
            m_Lights = scene.Lights();
            m_Environment = scene.Environment();
        }

        void SetCameraPose(const CameraPose& pose) { m_Camera.SetPose(pose); }
        void SetDirectionalShadowSettings(const DirectionalShadowSettings& settings)
        {
            settings.Validate();
            m_ShadowSettings = settings;
        }
        [[nodiscard]] const DirectionalShadowSettings& DirectionalShadows() const noexcept
        {
            return m_ShadowSettings;
        }
        void SetViewportShadingMode(ViewportShadingMode mode) noexcept
        {
            m_ShadingMode = mode;
        }
        [[nodiscard]] ViewportShadingMode ViewportShading() const noexcept
        {
            return m_ShadingMode;
        }

        void ResizeViewport(std::uint32_t width, std::uint32_t height)
        {
            if (width == 0u || height == 0u) return;
            if (width > 32'768u || height > 32'768u)
                throw std::invalid_argument(
                    "Metal viewport dimensions exceed 32768.");
            if (width == m_ViewportWidth && height == m_ViewportHeight) return;
            m_Backend.Resize(width, height);
            m_ViewportWidth = width;
            m_ViewportHeight = height;
            if (++m_ViewportGeneration == 0u) ++m_ViewportGeneration;
        }

        void RequestViewportPick(std::uint32_t x, std::uint32_t y)
        {
            if (x >= m_ViewportWidth || y >= m_ViewportHeight)
                throw std::out_of_range(
                    "Viewport pick lies outside the render target.");
            m_PickRequest = std::pair{x, y};
        }
        [[nodiscard]] std::optional<std::uint32_t> TakeViewportPickResult() noexcept
        {
            return std::exchange(m_PickResult, std::nullopt);
        }
        void RequestViewportCapture()
        {
            if (m_CaptureRequested)
                throw std::logic_error("A viewport capture is already pending.");
            m_CaptureRequested = true;
        }
        [[nodiscard]] std::optional<ViewportCapture> TakeViewportCapture() noexcept
        {
            return std::exchange(m_CaptureResult, std::nullopt);
        }

        [[nodiscard]] MetalViewportTexture ViewportTexture() const noexcept
        {
            return { m_Backend.ViewportTexture(), m_ViewportWidth,
                m_ViewportHeight, m_ViewportGeneration };
        }
        [[nodiscard]] MetalBackendContext BackendContext() const noexcept
        {
            return { m_Backend.Device() };
        }
        void SetOverlayRecorder(MetalOverlayRecorder recorder)
        {
            m_Backend.SetOverlayRecorder(std::move(recorder));
        }

    private:
        template<class MatrixType, std::size_t Count>
            requires (Count == 9u || Count == 16u)
        static void Copy(const MatrixType& source, float (&destination)[Count])
        {
            constexpr std::size_t Size = Count == 16u ? 4u : 3u;
            for (std::size_t row = 0u; row < Size; ++row)
                for (std::size_t column = 0u; column < Size; ++column)
                    destination[row * Size + column] = source(row, column);
        }

        static void Copy(const kairo::foundation::math::Vec3f& source,
            float (&destination)[3]) noexcept
        {
            destination[0] = source.x;
            destination[1] = source.y;
            destination[2] = source.z;
        }

        [[nodiscard]] static detail::MetalDraw Convert(const MeshDraw& draw)
        {
            detail::MetalDraw output;
            output.Mesh = draw.Mesh;
            Copy(draw.Model, output.Model);
            Copy(ComputeNormalMatrix(draw.Model), output.Normal);
            output.Material.BaseColor[0] = draw.Material.BaseColor.x;
            output.Material.BaseColor[1] = draw.Material.BaseColor.y;
            output.Material.BaseColor[2] = draw.Material.BaseColor.z;
            output.Material.BaseColor[3] = draw.Material.BaseColorAlpha;
            Copy(draw.Material.Emissive, output.Material.Emissive);
            output.Material.Metallic = draw.Material.Metallic;
            output.Material.Roughness = draw.Material.Roughness;
            output.Material.AmbientOcclusion = draw.Material.AmbientOcclusion;
            output.Material.NormalScale = draw.Material.NormalScale;
            output.Material.AlphaCutoff = draw.Material.AlphaCutoff;
            output.Material.AlphaMode = static_cast<std::uint32_t>(draw.Material.AlphaMode);
            output.Material.ObjectID = draw.ObjectID;
            output.Material.DoubleSided = draw.Material.DoubleSided;
            output.Material.ReceiveShadows = draw.ReceiveShadows;
            output.Material.BaseColorTexture = draw.Material.BaseColorTexture;
            output.Material.NormalTexture = draw.Material.NormalTexture;
            output.Material.MetallicRoughnessTexture = draw.Material.MetallicRoughnessTexture;
            output.Material.EmissiveTexture = draw.Material.EmissiveTexture;
            output.Material.OcclusionTexture = draw.Material.OcclusionTexture;
            output.CastShadows = draw.CastShadows;
            return output;
        }

        [[nodiscard]] static detail::MetalLight Convert(const RenderLight& light)
        {
            detail::MetalLight output;
            output.PositionType[0] = light.Position.x;
            output.PositionType[1] = light.Position.y;
            output.PositionType[2] = light.Position.z;
            output.PositionType[3] = static_cast<float>(light.Type);
            const auto direction = kairo::foundation::math::SafeNormalize(
                light.Direction, kairo::foundation::math::Vec3f::Up());
            output.DirectionRange[0] = direction.x;
            output.DirectionRange[1] = direction.y;
            output.DirectionRange[2] = direction.z;
            output.DirectionRange[3] = light.Range;
            output.ColorIntensity[0] = light.Color.x;
            output.ColorIntensity[1] = light.Color.y;
            output.ColorIntensity[2] = light.Color.z;
            output.ColorIntensity[3] = light.Intensity;
            output.SpotArea[0] = std::cos(light.InnerConeRadians);
            output.SpotArea[1] = std::cos(light.OuterConeRadians);
            output.SpotArea[2] = light.AreaWidth;
            output.SpotArea[3] = light.AreaHeight;
            output.CastShadows = light.CastShadows ? 1u : 0u;
            return output;
        }

        [[nodiscard]] static detail::MetalDebugVertex Convert(
            const kairo::foundation::math::Vec3f& position,
            const DebugColor& color) noexcept
        {
            return { { position.x, position.y, position.z },
                { color.R, color.G, color.B, color.A } };
        }

        [[nodiscard]] static detail::MetalTextureFormat Convert(
            kairo::assets::TexturePixelFormat format,
            kairo::assets::TextureColorSpace colorSpace)
        {
            using F = kairo::assets::TexturePixelFormat;
            if (format == F::R8) return detail::MetalTextureFormat::R8Linear;
            if (format == F::RG8) return detail::MetalTextureFormat::RG8Linear;
            if (format == F::RGBA16Float)
                return detail::MetalTextureFormat::RGBA16Float;
            if (format == F::RGBA8 &&
                colorSpace == kairo::assets::TextureColorSpace::SRGB)
                return detail::MetalTextureFormat::RGBA8SRGB;
            if (format == F::RGBA8) return detail::MetalTextureFormat::RGBA8Linear;
            throw std::invalid_argument("Unsupported Metal texture format.");
        }

        [[nodiscard]] static detail::MetalAddressMode Convert(
            TextureAddressMode mode)
        {
            switch (mode)
            {
                case TextureAddressMode::Repeat:
                    return detail::MetalAddressMode::Repeat;
                case TextureAddressMode::MirroredRepeat:
                    return detail::MetalAddressMode::Mirror;
                case TextureAddressMode::ClampToEdge:
                    return detail::MetalAddressMode::Clamp;
            }
            throw std::invalid_argument("Unsupported Metal texture address mode.");
        }

        void ValidateTexture(TextureHandle texture) const
        {
            if (texture != InvalidTextureHandle && !m_Textures.contains(texture))
                throw std::invalid_argument(
                    "RenderScene references a Metal texture owned by another runtime.");
        }

        [[nodiscard]] bool TextureIsSubmitted(TextureHandle texture) const noexcept
        {
            const auto uses = [texture](const PBRMaterial& material)
            {
                return material.BaseColorTexture == texture ||
                    material.NormalTexture == texture ||
                    material.MetallicRoughnessTexture == texture ||
                    material.EmissiveTexture == texture ||
                    material.OcclusionTexture == texture;
            };
            return m_Environment.EnvironmentTexture == texture ||
                std::ranges::any_of(m_Draws,
                    [&](const MeshDraw& draw) { return uses(draw.Material); });
        }

        [[nodiscard]] kairo::foundation::math::Mat4f
        BuildLightViewProjection() const
        {
            using namespace kairo::foundation::math;
            const auto found = std::ranges::find_if(m_Lights,
                [](const RenderLight& light)
                {
                    return light.Type == RenderLightType::Directional &&
                        light.CastShadows;
                });
            const Vec3f direction = found == m_Lights.end()
                ? Vec3f::Up()
                : SafeNormalize(found->Direction, Vec3f::Up());
            return Orthographic(-8.0f, 8.0f, -8.0f, 8.0f, 0.1f, 30.0f) *
                LookAt(direction * 12.0f, Vec3f::Zero(), Vec3f::Up());
        }
    };
}
