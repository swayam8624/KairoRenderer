#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace kairo::renderer::detail
{
    // Compare shader-visible float bits, not approximate vector/matrix
    // equality: even a small authored material or joint change must upload.
    [[nodiscard]] inline bool SameDescriptorFloat(float a, float b) noexcept
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    template<class Material>
    [[nodiscard]] bool SameDescriptorMaterial(const Material& a, const Material& b) noexcept
    {
        const auto same = SameDescriptorFloat;
        return same(a.BaseColor.x, b.BaseColor.x) &&
            same(a.BaseColor.y, b.BaseColor.y) && same(a.BaseColor.z, b.BaseColor.z) &&
            same(a.BaseColorAlpha, b.BaseColorAlpha) &&
            same(a.Emissive.x, b.Emissive.x) && same(a.Emissive.y, b.Emissive.y) &&
            same(a.Emissive.z, b.Emissive.z) && same(a.NormalScale, b.NormalScale) &&
            same(a.Metallic, b.Metallic) && same(a.Roughness, b.Roughness) &&
            same(a.AmbientOcclusion, b.AmbientOcclusion) &&
            same(a.AlphaCutoff, b.AlphaCutoff) && a.AlphaMode == b.AlphaMode &&
            a.DoubleSided == b.DoubleSided &&
            a.BaseColorTexture == b.BaseColorTexture && a.NormalTexture == b.NormalTexture &&
            a.MetallicRoughnessTexture == b.MetallicRoughnessTexture &&
            a.EmissiveTexture == b.EmissiveTexture && a.OcclusionTexture == b.OcclusionTexture;
    }

    template<class Palette>
    [[nodiscard]] bool SameDescriptorPalette(const Palette& a, const Palette& b) noexcept
    {
        if (a.Size() != b.Size()) return false;
        for (std::size_t joint = 0u; joint < a.Size(); ++joint)
            for (std::size_t row = 0u; row < 4u; ++row)
                for (std::size_t col = 0u; col < 4u; ++col)
                    if (!SameDescriptorFloat(a.JointMatrices[joint](row, col),
                        b.JointMatrices[joint](row, col))) return false;
        return true;
    }

    // Vulkan descriptors are indexed by draw position and depend on the PBR
    // factors, texture handles, palette and shared environment texture.
    // Model, picking ID, shadow flags, camera and light values are supplied
    // by other buffers/push constants. Do not invalidate for those changes.
    // Native resource replacement and failed rebuilds independently latch dirty.
    template<class Draws, class Texture>
    [[nodiscard]] bool DescriptorInputsChanged(const Draws& oldDraws, const Draws& newDraws,
        Texture oldEnvironment, Texture newEnvironment) noexcept
    {
        if (oldEnvironment != newEnvironment || oldDraws.size() != newDraws.size()) return true;
        for (std::size_t i = 0u; i < oldDraws.size(); ++i)
            if (!SameDescriptorMaterial(oldDraws[i].Material, newDraws[i].Material) ||
                !SameDescriptorPalette(oldDraws[i].Skinning, newDraws[i].Skinning)) return true;
        return false;
    }

    // A pending rebuild must survive further identical submissions, including
    // retry after native allocation failure. Only successful publication clears it.
    template<class Draws, class Texture>
    void InvalidateDescriptorInputs(bool& dirty, const Draws& oldDraws, const Draws& newDraws,
        Texture oldEnvironment, Texture newEnvironment) noexcept
    {
        dirty = dirty || DescriptorInputsChanged(oldDraws, newDraws, oldEnvironment, newEnvironment);
    }
}
