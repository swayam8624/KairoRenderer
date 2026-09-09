module;

#include <cstddef>
#include <cstdint>
#include <vector>

export module Kairo.Renderer.Telemetry;

import Kairo.Assets.TextureArtifact;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.RenderGraph;
import Kairo.Renderer.RenderScene;

export namespace kairo::renderer
{
    struct RendererSceneTelemetry final
    {
        std::uint64_t DrawCount = 0u;
        std::uint64_t SkinnedDrawCount = 0u;
        std::uint64_t ShadowCasterCount = 0u;
        std::uint64_t LightCount = 0u;
    };

    struct RendererResourceTelemetry final
    {
        std::uint64_t ResidentMeshCount = 0u;
        std::uint64_t ResidentTextureCount = 0u;
        std::uint64_t EstimatedResidentMeshBytes = 0u;
        std::uint64_t EstimatedResidentTextureBytes = 0u;
        std::uint64_t UploadBytesThisFrame = 0u;
    };

    /// Backend-neutral measurements that can be collected without pretending
    /// CPU estimates are native GPU counters. GPU timestamp/heap fields are
    /// intentionally absent until each backend provides real measurements.
    struct RendererFrameTelemetry final
    {
        std::uint64_t FrameNumber = 0u;
        double CpuFrameMilliseconds = 0.0;
        RenderGraphExecutionProfile CpuGraphProfile;
        RendererSceneTelemetry Scene;
        RendererResourceTelemetry Resources;
    };

    [[nodiscard]] inline std::uint64_t EstimateMeshResidentBytes(
        const Mesh& mesh) noexcept
    {
        return static_cast<std::uint64_t>(mesh.VertexBytes()) +
            static_cast<std::uint64_t>(mesh.IndexBytes()) +
            static_cast<std::uint64_t>(mesh.SkinBytes());
    }

    [[nodiscard]] inline std::uint64_t EstimateTextureResidentBytes(
        const kairo::assets::TextureArtifactData& texture) noexcept
    {
        std::uint64_t bytes = 0u;
        for (const auto& mip : texture.Mips)
            bytes += static_cast<std::uint64_t>(mip.Pixels.size());
        return bytes;
    }

    [[nodiscard]] inline RendererSceneTelemetry SummarizeRenderScene(
        const RenderScene& scene) noexcept
    {
        RendererSceneTelemetry result;
        result.DrawCount = static_cast<std::uint64_t>(scene.Draws().size());
        result.LightCount = static_cast<std::uint64_t>(scene.Lights().size());
        for (const auto& draw : scene.Draws())
        {
            if (!draw.Skinning.Empty()) ++result.SkinnedDrawCount;
            if (draw.CastShadows) ++result.ShadowCasterCount;
        }
        return result;
    }
}
