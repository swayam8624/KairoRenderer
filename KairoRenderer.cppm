// TEACHING ENTRY POINT: Public C++23 umbrella for the real-time renderer.
// This module *re-exports interfaces*; it does not create a window, select a
// GPU, or execute any passes. RendererRuntime (Renderer.cppm) is the facade;
// Vulkan/Metal/OpenGL/D3D12 backends own device, presentation, upload and
// readback lifetimes. Conditional exports depend on build flags, not wishes.
//
// Learning dependency flow: GraphicsBackend/Types define host and graphics
// contracts; RenderScene describes draws/lights; AssetAdapters bridges stable
// KairoAssets IDs into transient GPU handles; RenderGraph compiles logical
// resource hazards/schedules; RendererRuntime dispatches a frame through one
// *compiled* backend. EngineCore, Editor and Player are downstream clients.
//
// Be precise with telemetry: CpuFrameMilliseconds is not GPU elapsed time,
// CpuGraphProfile measures CPU callbacks, and resident bytes are estimates
// rather than authoritative native heap usage. See docs/SOURCE_READING_ORDER.md.
module;

#include <cstddef>
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
    // Values are extracted from submitted scene records; do not confuse
    // draw counts with a GPU command count or visible/pixel-contributing
    // object count. A draw may have zero visible fragments.
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

    // Summation of CPU-described vertex/index/skinning payload lengths.
    // Backend allocations may include alignment, staging and padding.
    [[nodiscard]] inline std::uint64_t EstimateMeshResidentBytes(
        const Mesh& mesh) noexcept
    {
        return static_cast<std::uint64_t>(mesh.VertexBytes()) +
            static_cast<std::uint64_t>(mesh.IndexBytes()) +
            static_cast<std::uint64_t>(mesh.SkinBytes());
    }

    // Scene snapshot counting without inspecting culling or GPU counters.
    // RendererRuntime/Editor can display this even when native backend
    // telemetry is unavailable or a render frame has not been submitted.
    [[nodiscard]] inline RendererSceneTelemetry SummarizeRenderScene(
        const RenderScene& scene) noexcept
    {
        RendererSceneTelemetry result;
        const auto& draws = scene.Draws();
        result.DrawCount = static_cast<std::uint64_t>(draws.size());
        result.LightCount = static_cast<std::uint64_t>(scene.Lights().size());
        for (std::size_t index = 0u; index < draws.size(); ++index)
        {
            const auto& draw = draws[index];
            if (!draw.Skinning.Empty()) ++result.SkinnedDrawCount;
            if (draw.CastShadows) ++result.ShadowCasterCount;
        }
        return result;
    }
}
