#pragma once

#include "MetalBackend.hpp"
#include <bit>
#include <cstdint>

namespace kairo::renderer::detail
{
    // Independent light-pass predicate; camera visibility never participates.
    [[nodiscard]] inline bool MetalShadowIncluded(std::size_t, const MetalDraw& draw) noexcept
    {
        return draw.CastShadows && draw.Material.AlphaMode != 3u;
    }

    // Every fragment-visible material factor and every native texture/cull
    // binding must agree. Object ID, model and receive-shadow flags travel
    // in the instance stream instead. Do not batch skinned/masked/blended draws.
    [[nodiscard]] inline bool MetalInstanceEligible(const MetalDraw& draw) noexcept
    {
        return draw.Material.AlphaMode == 1u && draw.SkinJointCount == 0u &&
            draw.SkinMatrices == nullptr;
    }

    [[nodiscard]] inline bool MetalInstanceCompatible(const MetalDraw& a,
        const MetalDraw& b) noexcept
    {
        if (!MetalInstanceEligible(a) || !MetalInstanceEligible(b) || a.Mesh != b.Mesh)
            return false;
        const auto& x = a.Material;
        const auto& y = b.Material;
        const auto same = [](float l,float r)
        {
            return std::bit_cast<std::uint32_t>(l) == std::bit_cast<std::uint32_t>(r);
        };
        for (unsigned i=0;i<4;++i)
            if (!same(x.BaseColor[i],y.BaseColor[i])) return false;
        for (unsigned i=0;i<3;++i)
            if (!same(x.Emissive[i],y.Emissive[i])) return false;
        return same(x.Metallic,y.Metallic) && same(x.Roughness,y.Roughness) &&
            same(x.AmbientOcclusion,y.AmbientOcclusion) &&
            same(x.NormalScale,y.NormalScale) && same(x.AlphaCutoff,y.AlphaCutoff) &&
            x.DoubleSided == y.DoubleSided &&
            x.BaseColorTexture == y.BaseColorTexture && x.NormalTexture == y.NormalTexture &&
            x.MetallicRoughnessTexture == y.MetallicRoughnessTexture &&
            x.EmissiveTexture == y.EmissiveTexture && x.OcclusionTexture == y.OcclusionTexture;
    }
}
