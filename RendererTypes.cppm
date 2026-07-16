module;

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

export module Kairo.Renderer.Types;

export namespace kairo::renderer
{
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
