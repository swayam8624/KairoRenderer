module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module Kairo.Renderer.Types;

import Kairo.Renderer.GraphicsBackend;

export namespace kairo::renderer
{
    /// Signals that the selected backend exists but the current host cannot
    /// create a presentation surface. CI may skip native smoke tests while
    /// retaining renderer contract validation.
    class PresentationUnavailableError final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// Backend-neutral RGBA8 readback used by screenshots and native smoke
    /// tests. Every backend must return top-left-origin pixels in row-major
    /// order so callers never branch on graphics API conventions.
    struct ViewportCapture final
    {
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::vector<std::uint8_t> RGBA;

        [[nodiscard]] bool IsVisuallyNonUniform(
            std::uint8_t minimumRange = 4u) const noexcept
        {
            if (RGBA.size() != static_cast<std::size_t>(Width) * Height * 4u ||
                RGBA.empty()) return false;
            std::uint8_t minimum = 255u;
            std::uint8_t maximum = 0u;
            for (std::size_t index = 0u; index < RGBA.size(); index += 4u)
                for (std::size_t channel = 0u; channel < 3u; ++channel)
                {
                    minimum = std::min(minimum, RGBA[index + channel]);
                    maximum = std::max(maximum, RGBA[index + channel]);
                }
            return static_cast<unsigned>(maximum) - minimum >= minimumRange;
        }
    };

    /// Non-owning OpenGL viewport identity consumed by native tooling. The
    /// generation changes whenever resize replaces the texture object, so UI
    /// adapters never retain a stale name.
    struct OpenGLViewportTexture final
    {
        std::uint32_t Name = 0u;
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::uint64_t Generation = 0u;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return Name != 0u && Width != 0u && Height != 0u && Generation != 0u;
        }
    };

    /// Tooling callback invoked after OpenGL scene presentation and before
    /// buffer swap. It mirrors VulkanOverlayRecorder without leaking GL names
    /// into Editor-independent scene extraction.
    using OpenGLOverlayRecorder = std::function<void()>;

    /// Borrowed Metal viewport texture and device identities for tooling. The
    /// pointers are Objective-C objects erased at the C++ module boundary;
    /// clients must use the platform adapter and never retain or release them.
    struct MetalViewportTexture final
    {
        void* Texture = nullptr;
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::uint64_t Generation = 0u;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return Texture != nullptr && Width != 0u && Height != 0u &&
                Generation != 0u;
        }
    };

    struct MetalBackendContext final
    {
        void* Device = nullptr;
        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return Device != nullptr;
        }
    };

    using MetalOverlayRecorder =
        std::function<void(void* commandBuffer, void* renderEncoder)>;

    /// Direct3D 12 descriptors are represented by their SDK-defined integer
    /// handle values at the module boundary. The Windows Editor adapter turns
    /// them back into D3D12 handles; portable scene/runtime code never sees a
    /// DirectX declaration.
    struct Direct3D12Descriptor final
    {
        std::uintptr_t CPU = 0u;
        std::uint64_t GPU = 0u;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return CPU != 0u && GPU != 0u;
        }
    };

    struct Direct3D12ViewportTexture final
    {
        Direct3D12Descriptor Descriptor;
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::uint64_t Generation = 0u;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return Descriptor.IsValid() && Width != 0u && Height != 0u &&
                Generation != 0u;
        }
    };

    struct Direct3D12BackendContext final
    {
        void* Device = nullptr;
        void* CommandQueue = nullptr;
        void* ShaderResourceHeap = nullptr;
        std::uint32_t FramesInFlight = 0u;
        std::uint32_t RenderTargetFormat = 0u;
        std::uint32_t DepthStencilFormat = 0u;
        std::function<Direct3D12Descriptor()> AllocateDescriptor;
        std::function<void(Direct3D12Descriptor)> FreeDescriptor;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return Device != nullptr && CommandQueue != nullptr &&
                ShaderResourceHeap != nullptr && FramesInFlight != 0u &&
                RenderTargetFormat != 0u && AllocateDescriptor && FreeDescriptor;
        }
    };

    using Direct3D12OverlayRecorder = std::function<void(void* commandList)>;

    /// Fragment presentation used by editor and diagnostic viewports. Values
    /// are part of the CPU/GPU frame contract and must remain stable.
    enum class ViewportShadingMode : std::uint32_t
    {
        Lit = 0u,
        Unlit = 1u,
        Normals = 2u,
        Lighting = 3u
    };

    [[nodiscard]] constexpr std::string_view Name(ViewportShadingMode mode) noexcept
    {
        switch (mode)
        {
            case ViewportShadingMode::Lit: return "Lit";
            case ViewportShadingMode::Unlit: return "Unlit";
            case ViewportShadingMode::Normals: return "Normals";
            case ViewportShadingMode::Lighting: return "Lighting";
        }
        return "Unknown";
    }

    /// Input: window/application configuration.
    /// Output: deterministic native-window creation settings.
    /// Task: keep editor, samples, and future EngineCore applications on one
    /// rendering surface contract rather than each embedding GLFW calls.
    struct WindowDesc final
    {
        std::string Title = "KairoRenderer";
        std::uint32_t Width = 1280;
        std::uint32_t Height = 800;
        bool Resizable = true;
        GraphicsBackend Backend = GraphicsBackend::Automatic;
    };

    inline void ValidateWindowDesc(const WindowDesc& desc)
    {
        if (desc.Title.empty() || desc.Width == 0u || desc.Height == 0u)
        {
            throw std::invalid_argument("WindowDesc requires a non-empty title and non-zero extent.");
        }
    }

    /// Input: Vulkan setup policy.
    /// Output: an instance configuration suitable for MoltenVK portability.
    /// Task: centralize explicit validation and portability behavior before
    /// device/swapchain work begins.
    struct VulkanInstanceDesc final
    {
        std::string ApplicationName = "KairoRenderer";
        bool EnableValidation = true;
    };
}
