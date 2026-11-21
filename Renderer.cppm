// TEACHING MAP: RENDERER FACADE: The C++23 public runtime module delegates backend-neutral scene, render and debug operations to Vulkan, Metal, OpenGL or D3D12 through a tagged variant. Imports name interface dependencies. Actual device/swapchain lifetime remains inside each backend. Player and Editor consume this façade; native backend tests validate platform execution.
module;

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

export module Kairo.Renderer.Runtime;

import Kairo.Renderer.Types;
import Kairo.Renderer.RenderGraph;
import Kairo.Renderer.GraphicsBackend;
import Kairo.Renderer.Camera;
import Kairo.Renderer.Window;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.Texture;
import Kairo.Renderer.RenderScene;
import Kairo.Renderer.ShadowSettings;
import Kairo.Renderer.DebugDraw;
import Kairo.Renderer.VulkanInstance;
import Kairo.Renderer.VulkanViewportTarget;
import Kairo.Renderer.VulkanBackendContext;
import Kairo.Renderer.VulkanRuntime;
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
import Kairo.Renderer.OpenGLRuntime;
#endif
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
import Kairo.Renderer.MetalRuntime;
#endif
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
import Kairo.Renderer.Direct3D12Runtime;
#endif
import Kairo.Assets.TextureArtifact;

export namespace kairo::renderer
{
    /// Backend-selecting façade used by games, Player, Editor, and tools.
    ///
    /// Input: one validated WindowDesc with an automatic or explicit backend.
    /// Output: a concrete compiled runtime hidden behind a stable rendering API.
    /// Task: keep asset handles, scene extraction, camera state, debug drawing,
    /// capture, and picking identical across graphics APIs. Native tooling
    /// accessors remain Vulkan compatibility points until the Editor tooling
    /// bridge is converted to a backend-context variant.
    class RendererRuntime final
    {
        using BackendRuntime = std::variant<
            std::unique_ptr<VulkanRendererRuntime>
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
            , std::unique_ptr<OpenGLRendererRuntime>
#endif
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
            , std::unique_ptr<MetalRendererRuntime>
#endif
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
            , std::unique_ptr<Direct3D12RendererRuntime>
#endif
        >;

        GraphicsBackend m_Backend;
        BackendRuntime m_Runtime;
        RenderGraphExecutionProfile m_LastFrameProfile;

// Central dispatch visits the actually chosen backend. The shared API does not make
// platform-native GPU objects interchangeable.
        template<class Operation>
        /// TEACHING METHOD: The backend-agnostic dispatcher uses std::variant + std::visit; runtime implementation owns native device and handles. Both const and mutable overloads preserve caller constness.
        decltype(auto) Visit(Operation&& operation)
        {
            return std::visit([&](auto& runtime) -> decltype(auto)
            {
                return std::forward<Operation>(operation)(*runtime);
            }, m_Runtime);
        }

        template<class Operation>
        /// TEACHING METHOD: The backend-agnostic dispatcher uses std::variant + std::visit; runtime implementation owns native device and handles. Both const and mutable overloads preserve caller constness.
        decltype(auto) Visit(Operation&& operation) const
        {
            return std::visit([&](const auto& runtime) -> decltype(auto)
            {
                return std::forward<Operation>(operation)(*runtime);
            }, m_Runtime);
        }

    public:
        /// TEACHING METHOD: Validate/select an available backend from WindowDesc and instantiate it through CreateBackend; explicit unavailable choices should fail, not silently change APIs.
        explicit RendererRuntime(const WindowDesc& windowDesc)
            : m_Backend(SelectGraphicsBackend(
                  windowDesc.Backend, CompiledGraphicsBackends())),
              /// TEACHING METHOD: Factory for the explicitly chosen API; compile-time feature guards prevent nonexistent native implementations from being constructed.
              m_Runtime(CreateBackend(m_Backend, windowDesc))
        {
        }

        /// TEACHING METHOD: Return the selected API for diagnostics and feature-aware adapters.
        [[nodiscard]] GraphicsBackend Backend() const noexcept { return m_Backend; }

        /// TEACHING METHOD: Delegate a full present/render cycle and record that backend's completed CPU render-stage profile.
        void DrawFrame()
        {
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
            if (m_Backend == GraphicsBackend::OpenGL)
            {
                OpenGLRendererRuntime& runtime = OpenGL();
                runtime.DrawFrame();
                /// TEACHING METHOD: Snapshot of completed render stages for Editor profiling; timings are backend-specific CPU measurements, not cross-API GPU timestamp parity.
                m_LastFrameProfile = runtime.LastFrameProfile();
                return;
            }
#endif
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
            if (m_Backend == GraphicsBackend::Metal)
            {
                MetalRendererRuntime& runtime = Metal();
                runtime.DrawFrame();
                m_LastFrameProfile = runtime.LastFrameProfile();
                return;
            }
#endif
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
            if (m_Backend == GraphicsBackend::Direct3D12)
            {
                Direct3D12RendererRuntime& runtime = Direct3D12();
                runtime.DrawFrame();
                m_LastFrameProfile = runtime.LastFrameProfile();
                return;
            }
#endif
            VulkanRendererRuntime& runtime = Vulkan();
            runtime.DrawFrame();
            m_LastFrameProfile = runtime.LastFrameProfile();
        }

        /// Output: CPU-side timings for the real graph-owned stages executed by
        /// the selected backend. Stage granularity is intentionally backend-
        /// specific: OpenGL exposes fine render phases, Vulkan exposes queue/
        /// presentation lifecycle phases, and Metal/D3D12 expose their native
        /// render-present plus synchronous readback boundaries.
        [[nodiscard]] const RenderGraphExecutionProfile& LastFrameProfile()
            const noexcept
        {
            return m_LastFrameProfile;
        }

        /// TEACHING METHOD: Expose the backend's actual native window abstraction to Player/Editor event loops.
        [[nodiscard]] Window& NativeWindow() noexcept
        {
            return Visit([](auto& runtime) -> Window& { return runtime.NativeWindow(); });
        }

        /// TEACHING METHOD: Forward world-space debug lines from physics/editor to the selected renderer.
        void SubmitDebugDraw(const DebugDrawList& debug)
        {
            Visit([&](auto& runtime) { runtime.SubmitDebugDraw(debug); });
        }

        /// TEACHING METHOD: Create backend-owned GPU/indexed mesh resources from a validated portable Mesh contract; handle lifetime must be released with DestroyMesh.
        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
            return Visit([&](auto& runtime) { return runtime.CreateMesh(mesh); });
        }

        /// TEACHING METHOD: Retire an existing backend mesh handle. Handle validity and resource synchronization are native-backend concerns.
        void DestroyMesh(MeshHandle mesh)
        {
            Visit([&](auto& runtime) { runtime.DestroyMesh(mesh); });
        }

        /// TEACHING METHOD: Convert KairoAssets texture artifact and sampler policy into a backend texture handle, preserving authored color-space and filtering semantics.
        [[nodiscard]] TextureHandle CreateTexture(
            const kairo::assets::TextureArtifactData& texture,
            TextureSampler sampling = {})
        {
            return Visit([&](auto& runtime)
            {
                return runtime.CreateTexture(texture, sampling);
            });
        }

        /// TEACHING METHOD: Release a backend-owned texture resource; scene bindings must no longer reference the handle.
        void DestroyTexture(TextureHandle texture)
        {
            Visit([&](auto& runtime) { runtime.DestroyTexture(texture); });
        }

        /// TEACHING METHOD: Hand the same authored RenderScene to each native implementation; renderer-specific draw extraction occurs downstream.
        void SubmitRenderScene(const RenderScene& scene)
        {
            Visit([&](auto& runtime) { runtime.SubmitRenderScene(scene); });
        }

        /// TEACHING METHOD: Set viewport camera transform independently of asset loading or scene serialization.
        void SetCameraPose(const CameraPose& pose)
        {
            Visit([&](auto& runtime) { runtime.SetCameraPose(pose); });
        }

        /// TEACHING METHOD: Forward validated shadow bias/filter settings to the backend. Shadow maps are rendered in backend passes, not by the Editor.
        void SetDirectionalShadowSettings(
            const DirectionalShadowSettings& settings)
        {
            Visit([&](auto& runtime)
            {
                runtime.SetDirectionalShadowSettings(settings);
            });
        }

        /// TEACHING METHOD: Read active settings from the backend for UI inspection and deterministic round trips.
        [[nodiscard]] const DirectionalShadowSettings& DirectionalShadows() const noexcept
        {
            return Visit([](const auto& runtime)
                -> const DirectionalShadowSettings&
            {
                return runtime.DirectionalShadows();
            });
        }

        /// TEACHING METHOD: Switch viewport display/debug shading without altering authored PBR material data.
        void SetViewportShadingMode(ViewportShadingMode mode) noexcept
        {
            Visit([&](auto& runtime) { runtime.SetViewportShadingMode(mode); });
        }

        /// TEACHING METHOD: Report active viewport shading mode to Editor tools.
        [[nodiscard]] ViewportShadingMode ViewportShading() const noexcept
        {
            return Visit([](const auto& runtime)
            {
                return runtime.ViewportShading();
            });
        }

        /// TEACHING METHOD: Request native render-target/swapchain-compatible viewport size; implementations manage GPU resource reallocation.
        void ResizeViewport(std::uint32_t width, std::uint32_t height)
        {
            Visit([&](auto& runtime) { runtime.ResizeViewport(width, height); });
        }

        /// TEACHING METHOD: Ask the GPU viewport to resolve an object ID at pixel coordinates; result is asynchronous or deferred.
        void RequestViewportPick(std::uint32_t x, std::uint32_t y)
        {
            Visit([&](auto& runtime) { runtime.RequestViewportPick(x, y); });
        }

        [[nodiscard]] std::optional<std::uint32_t>
        /// TEACHING METHOD: Consume the optional completed object-ID readback for Editor selection.
        TakeViewportPickResult() noexcept
        {
            return Visit([](auto& runtime)
            {
                return runtime.TakeViewportPickResult();
            });
        }

        /// TEACHING METHOD: Schedule viewport readback for smoke tests or visual inspection, not an immediate screen image.
        void RequestViewportCapture()
        {
            Visit([](auto& runtime) { runtime.RequestViewportCapture(); });
        }

        [[nodiscard]] std::optional<ViewportCapture>
        /// TEACHING METHOD: Take optional completed image data; the Player smoke fixture checks it for nonblank content.
        TakeViewportCapture() noexcept
        {
            return Visit([](auto& runtime)
            {
                return runtime.TakeViewportCapture();
            });
        }

        /// Vulkan compatibility surface used by the current Dear ImGui adapter.
        /// These accessors are deliberately isolated from the backend-neutral
        /// methods above and will be replaced by typed tooling-context variants
        /// as each native Editor backend lands.
        [[nodiscard]] const VulkanInstance& Instance() const
        {
            return Vulkan().Instance();
        }

        [[nodiscard]] VulkanViewportTexture ViewportTexture() const
        {
            return Vulkan().ViewportTexture();
        }

        void SetOverlayRecorder(VulkanOverlayRecorder recorder)
        {
            Vulkan().SetOverlayRecorder(std::move(recorder));
        }

        [[nodiscard]] VulkanBackendContext BackendContext() const
        {
            return Vulkan().BackendContext();
        }

        /// OpenGL tooling compatibility used by the Dear ImGui adapter. Scene
        /// extraction and game runtime code should remain on the neutral API.
        [[nodiscard]] OpenGLViewportTexture OpenGLViewportTextureInfo() const
        {
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
            return OpenGL().ViewportTexture();
#else
            throw std::logic_error(
                "OpenGL tooling access is unavailable in this renderer build.");
#endif
        }

        void SetOpenGLOverlayRecorder(OpenGLOverlayRecorder recorder)
        {
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
            OpenGL().SetOverlayRecorder(std::move(recorder));
#else
            if (recorder)
                throw std::logic_error(
                    "OpenGL tooling access is unavailable in this renderer build.");
#endif
        }

        [[nodiscard]] MetalViewportTexture MetalViewportTextureInfo() const
        {
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
            return Metal().ViewportTexture();
#else
            throw std::logic_error(
                "Metal tooling access is unavailable in this renderer build.");
#endif
        }

        [[nodiscard]] MetalBackendContext MetalContext() const
        {
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
            return Metal().BackendContext();
#else
            throw std::logic_error(
                "Metal tooling access is unavailable in this renderer build.");
#endif
        }

        void SetMetalOverlayRecorder(MetalOverlayRecorder recorder)
        {
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
            Metal().SetOverlayRecorder(std::move(recorder));
#else
            if (recorder)
                throw std::logic_error(
                    "Metal tooling access is unavailable in this renderer build.");
#endif
        }

        [[nodiscard]] Direct3D12ViewportTexture
        Direct3D12ViewportTextureInfo() const
        {
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
            return Direct3D12().ViewportTexture();
#else
            throw std::logic_error(
                "Direct3D 12 tooling access is unavailable in this renderer build.");
#endif
        }

        [[nodiscard]] Direct3D12BackendContext Direct3D12Context() const
        {
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
            return Direct3D12().BackendContext();
#else
            throw std::logic_error(
                "Direct3D 12 tooling access is unavailable in this renderer build.");
#endif
        }

        void SetDirect3D12OverlayRecorder(Direct3D12OverlayRecorder recorder)
        {
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
            Direct3D12().SetOverlayRecorder(std::move(recorder));
#else
            if (recorder)
                throw std::logic_error(
                    "Direct3D 12 tooling access is unavailable in this renderer build.");
#endif
        }

    private:
        [[nodiscard]] static BackendRuntime CreateBackend(
            GraphicsBackend backend, WindowDesc windowDesc)
        {
            windowDesc.Backend = backend;
            if (backend == GraphicsBackend::Vulkan)
                return BackendRuntime{ std::in_place_type<
                    std::unique_ptr<VulkanRendererRuntime>>,
                    std::make_unique<VulkanRendererRuntime>(windowDesc) };
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
            if (backend == GraphicsBackend::OpenGL)
                return BackendRuntime{ std::in_place_type<
                    std::unique_ptr<OpenGLRendererRuntime>>,
                    std::make_unique<OpenGLRendererRuntime>(windowDesc) };
#endif
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
            if (backend == GraphicsBackend::Metal)
                return BackendRuntime{ std::in_place_type<
                    std::unique_ptr<MetalRendererRuntime>>,
                    std::make_unique<MetalRendererRuntime>(windowDesc) };
#endif
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
            if (backend == GraphicsBackend::Direct3D12)
                return BackendRuntime{ std::in_place_type<
                    std::unique_ptr<Direct3D12RendererRuntime>>,
                    std::make_unique<Direct3D12RendererRuntime>(windowDesc) };
#endif
            throw std::logic_error(
                "Selected graphics backend has no runtime factory.");
        }

        [[nodiscard]] VulkanRendererRuntime& Vulkan()
        {
            if (m_Backend != GraphicsBackend::Vulkan)
                throw std::logic_error(
                    "Vulkan tooling access was requested from a non-Vulkan backend.");
            return *std::get<std::unique_ptr<VulkanRendererRuntime>>(m_Runtime);
        }

        [[nodiscard]] const VulkanRendererRuntime& Vulkan() const
        {
            if (m_Backend != GraphicsBackend::Vulkan)
                throw std::logic_error(
                    "Vulkan tooling access was requested from a non-Vulkan backend.");
            return *std::get<std::unique_ptr<VulkanRendererRuntime>>(m_Runtime);
        }

#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
        [[nodiscard]] OpenGLRendererRuntime& OpenGL()
        {
            if (m_Backend != GraphicsBackend::OpenGL)
                throw std::logic_error(
                    "OpenGL tooling access was requested from a non-OpenGL backend.");
            return *std::get<std::unique_ptr<OpenGLRendererRuntime>>(m_Runtime);
        }

        [[nodiscard]] const OpenGLRendererRuntime& OpenGL() const
        {
            if (m_Backend != GraphicsBackend::OpenGL)
                throw std::logic_error(
                    "OpenGL tooling access was requested from a non-OpenGL backend.");
            return *std::get<std::unique_ptr<OpenGLRendererRuntime>>(m_Runtime);
        }
#endif

#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
        [[nodiscard]] MetalRendererRuntime& Metal()
        {
            if (m_Backend != GraphicsBackend::Metal)
                throw std::logic_error(
                    "Metal tooling access was requested from a non-Metal backend.");
            return *std::get<std::unique_ptr<MetalRendererRuntime>>(m_Runtime);
        }

        [[nodiscard]] const MetalRendererRuntime& Metal() const
        {
            if (m_Backend != GraphicsBackend::Metal)
                throw std::logic_error(
                    "Metal tooling access was requested from a non-Metal backend.");
            return *std::get<std::unique_ptr<MetalRendererRuntime>>(m_Runtime);
        }
#endif

#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
        [[nodiscard]] Direct3D12RendererRuntime& Direct3D12()
        {
            if (m_Backend != GraphicsBackend::Direct3D12)
                throw std::logic_error(
                    "Direct3D 12 tooling access was requested from another backend.");
            return *std::get<std::unique_ptr<Direct3D12RendererRuntime>>(m_Runtime);
        }

        [[nodiscard]] const Direct3D12RendererRuntime& Direct3D12() const
        {
            if (m_Backend != GraphicsBackend::Direct3D12)
                throw std::logic_error(
                    "Direct3D 12 tooling access was requested from another backend.");
            return *std::get<std::unique_ptr<Direct3D12RendererRuntime>>(m_Runtime);
        }
#endif
    };
}
