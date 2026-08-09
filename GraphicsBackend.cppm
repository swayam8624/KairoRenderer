module;

#include <stdexcept>
#include <string>
#include <string_view>

export module Kairo.Renderer.GraphicsBackend;

export namespace kairo::renderer
{
    /// Native desktop family used when resolving an automatic graphics API.
    /// Keeping this explicit makes the policy testable without pretending the
    /// machine running a unit test is a different operating system.
    enum class HostPlatform
    {
        Unsupported,
        MacOS,
        Windows,
        Linux
    };

    /// Public graphics API choice. Automatic is a request policy and is never
    /// returned by SelectGraphicsBackend.
    enum class GraphicsBackend
    {
        Automatic,
        Vulkan,
        Metal,
        Direct3D12,
        OpenGL
    };

    /// Backends compiled into one KairoRenderer binary. Platform compatibility
    /// and build availability are separate: Metal is valid on macOS, for
    /// example, but cannot be selected until its implementation is linked.
    struct GraphicsBackendAvailability final
    {
        bool Vulkan = false;
        bool Metal = false;
        bool Direct3D12 = false;
        bool OpenGL = false;

        [[nodiscard]] constexpr bool Contains(GraphicsBackend backend) const noexcept
        {
            switch (backend)
            {
                case GraphicsBackend::Vulkan: return Vulkan;
                case GraphicsBackend::Metal: return Metal;
                case GraphicsBackend::Direct3D12: return Direct3D12;
                case GraphicsBackend::OpenGL: return OpenGL;
                case GraphicsBackend::Automatic: return false;
            }
            return false;
        }
    };

    [[nodiscard]] constexpr std::string_view Name(GraphicsBackend backend) noexcept
    {
        switch (backend)
        {
            case GraphicsBackend::Automatic: return "Automatic";
            case GraphicsBackend::Vulkan: return "Vulkan";
            case GraphicsBackend::Metal: return "Metal";
            case GraphicsBackend::Direct3D12: return "Direct3D12";
            case GraphicsBackend::OpenGL: return "OpenGL";
        }
        return "Unknown";
    }

    /// Input: case-sensitive stable command/config spelling.
    /// Output: the corresponding backend request.
    /// Task: centralize project, command-line, and package parsing so hosts do
    /// not invent incompatible aliases. Invalid values fail before window or
    /// graphics-device initialization.
    [[nodiscard]] inline GraphicsBackend ParseGraphicsBackend(std::string_view text)
    {
        if (text == "auto") return GraphicsBackend::Automatic;
        if (text == "vulkan") return GraphicsBackend::Vulkan;
        if (text == "metal") return GraphicsBackend::Metal;
        if (text == "d3d12" || text == "direct3d12") return GraphicsBackend::Direct3D12;
        if (text == "opengl") return GraphicsBackend::OpenGL;
        throw std::invalid_argument("Unknown graphics backend '" + std::string(text) +
            "'; expected auto, vulkan, metal, d3d12, or opengl.");
    }

    [[nodiscard]] constexpr HostPlatform CurrentHostPlatform() noexcept
    {
#if defined(_WIN32)
        return HostPlatform::Windows;
#elif defined(__APPLE__)
        return HostPlatform::MacOS;
#elif defined(__linux__)
        return HostPlatform::Linux;
#else
        return HostPlatform::Unsupported;
#endif
    }

    [[nodiscard]] constexpr bool IsPlatformCompatible(
        GraphicsBackend backend, HostPlatform platform) noexcept
    {
        switch (backend)
        {
            case GraphicsBackend::Automatic: return true;
            case GraphicsBackend::Vulkan:
            case GraphicsBackend::OpenGL:
                return platform == HostPlatform::MacOS ||
                    platform == HostPlatform::Windows || platform == HostPlatform::Linux;
            case GraphicsBackend::Metal: return platform == HostPlatform::MacOS;
            case GraphicsBackend::Direct3D12: return platform == HostPlatform::Windows;
        }
        return false;
    }

    /// Output: the native-first policy before compiled availability is applied.
    /// macOS prefers Metal, Windows prefers Direct3D12, and Linux prefers
    /// Vulkan. OpenGL remains the portable compatibility fallback.
    [[nodiscard]] constexpr GraphicsBackend PreferredGraphicsBackend(HostPlatform platform) noexcept
    {
        switch (platform)
        {
            case HostPlatform::MacOS: return GraphicsBackend::Metal;
            case HostPlatform::Windows: return GraphicsBackend::Direct3D12;
            case HostPlatform::Linux: return GraphicsBackend::Vulkan;
            case HostPlatform::Unsupported: return GraphicsBackend::Automatic;
        }
        return GraphicsBackend::Automatic;
    }

    /// Input: requested policy, linked implementations, and target platform.
    /// Output: one concrete, compatible, compiled graphics API.
    /// Task: make backend choice deterministic and fail early. Automatic uses
    /// native-first order: Metal/Vulkan/OpenGL on macOS, D3D12/Vulkan/OpenGL on
    /// Windows, and Vulkan/OpenGL on Linux.
    [[nodiscard]] inline GraphicsBackend SelectGraphicsBackend(
        GraphicsBackend requested,
        const GraphicsBackendAvailability& available,
        HostPlatform platform = CurrentHostPlatform())
    {
        if (platform == HostPlatform::Unsupported)
            throw std::runtime_error("KairoRenderer does not support this host platform.");

        if (requested != GraphicsBackend::Automatic)
        {
            if (!IsPlatformCompatible(requested, platform))
                throw std::invalid_argument(std::string(Name(requested)) +
                    " is not supported on the selected host platform.");
            if (!available.Contains(requested))
                throw std::runtime_error(std::string(Name(requested)) +
                    " was requested but is not compiled into KairoRenderer.");
            return requested;
        }

        const GraphicsBackend native = PreferredGraphicsBackend(platform);
        if (available.Contains(native)) return native;
        if (available.Vulkan && IsPlatformCompatible(GraphicsBackend::Vulkan, platform))
            return GraphicsBackend::Vulkan;
        if (available.OpenGL && IsPlatformCompatible(GraphicsBackend::OpenGL, platform))
            return GraphicsBackend::OpenGL;
        if (available.Metal && IsPlatformCompatible(GraphicsBackend::Metal, platform))
            return GraphicsBackend::Metal;
        if (available.Direct3D12 && IsPlatformCompatible(GraphicsBackend::Direct3D12, platform))
            return GraphicsBackend::Direct3D12;
        throw std::runtime_error("No compatible graphics backend is compiled into KairoRenderer.");
    }

    /// Output: availability encoded by this binary's build configuration.
    [[nodiscard]] constexpr GraphicsBackendAvailability CompiledGraphicsBackends() noexcept
    {
        GraphicsBackendAvailability result;
#if defined(KAIRO_RENDERER_HAS_VULKAN_BACKEND)
        result.Vulkan = true;
#endif
#if defined(KAIRO_RENDERER_HAS_METAL_BACKEND)
        result.Metal = true;
#endif
#if defined(KAIRO_RENDERER_HAS_D3D12_BACKEND)
        result.Direct3D12 = true;
#endif
#if defined(KAIRO_RENDERER_HAS_OPENGL_BACKEND)
        result.OpenGL = true;
#endif
        return result;
    }
}
