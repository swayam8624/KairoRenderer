module;

#include <cstdint>

export module Kairo.Renderer;
export import Kairo.Renderer.GraphicsBackend;
export import Kairo.Renderer.Types;
export import Kairo.Renderer.RenderGraph;
export import Kairo.Renderer.Camera;
export import Kairo.Renderer.Mesh;
export import Kairo.Renderer.Skinning;
export import Kairo.Renderer.Material;
export import Kairo.Renderer.Texture;
export import Kairo.Renderer.AssetAdapters;
export import Kairo.Renderer.ShadowSettings;
export import Kairo.Renderer.RenderScene;
export import Kairo.Renderer.DebugDraw;
export import Kairo.Renderer.Window;
export import Kairo.Renderer.VulkanInstance;
export import Kairo.Renderer.VulkanSurface;
export import Kairo.Renderer.VulkanDevice;
export import Kairo.Renderer.VulkanSwapchain;
export import Kairo.Renderer.VulkanCommand;
export import Kairo.Renderer.VulkanSync;
export import Kairo.Renderer.VulkanBuffer;
export import Kairo.Renderer.VulkanDescriptor;
export import Kairo.Renderer.VulkanDepth;
export import Kairo.Renderer.VulkanViewportTarget;
export import Kairo.Renderer.VulkanShadowMap;
export import Kairo.Renderer.VulkanBackendContext;
export import Kairo.Renderer.VulkanTriangle;
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
export import Kairo.Renderer.OpenGLRuntime;
#endif
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
export import Kairo.Renderer.MetalRuntime;
#endif
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
export import Kairo.Renderer.Direct3D12Runtime;
#endif
export import Kairo.Renderer.Runtime;

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

    /// Measurement contract shared by runtime, editor profiling and future
    /// backend-native GPU counters. CPU graph timings remain explicitly CPU;
    /// memory fields remain explicitly estimated until native heap-budget APIs
    /// are wired for Vulkan/Metal/D3D12/OpenGL.
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
