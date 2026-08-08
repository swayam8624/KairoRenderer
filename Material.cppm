module;

#include <cmath>
#include <cstdint>
#include <stdexcept>

export module Kairo.Renderer.Material;

import Kairo.Foundation.Math;
import Kairo.Renderer.Texture;

export namespace kairo::renderer
{
    enum class MaterialAlphaMode : std::uint8_t
    {
        Opaque = 1u,
        Mask = 2u,
        Blend = 3u
    };

    /// Renderer-neutral metallic-roughness material consumed by the real-time
    /// forward pass. Values are linear, not display-encoded. Texture handles
    /// are optional process-local bindings resolved from persistent KairoAssets
    /// references by Editor or Player adapters.
    ///
    /// BaseColor: non-negative linear RGB multiplier applied to vertex color.
    /// Metallic: 0 for dielectric, 1 for conductor, blends allowed in between.
    /// Roughness: perceptual roughness in [0.04, 1]; the lower bound avoids a
    /// numerically singular microfacet distribution without hiding a delta BRDF.
    /// AmbientOcclusion: indirect-light visibility in [0, 1].
    struct PBRMaterial final
    {
        kairo::foundation::math::Vec3f BaseColor{ 1.0f, 1.0f, 1.0f };
        float BaseColorAlpha = 1.0f;
        float Metallic = 0.0f;
        float Roughness = 0.55f;
        float AmbientOcclusion = 1.0f;
        kairo::foundation::math::Vec3f Emissive{};
        float NormalScale = 1.0f;
        float AlphaCutoff = 0.5f;
        MaterialAlphaMode AlphaMode = MaterialAlphaMode::Opaque;
        bool DoubleSided = false;
        TextureHandle BaseColorTexture = InvalidTextureHandle;
        TextureHandle NormalTexture = InvalidTextureHandle;
        TextureHandle MetallicRoughnessTexture = InvalidTextureHandle;
        TextureHandle EmissiveTexture = InvalidTextureHandle;
        TextureHandle OcclusionTexture = InvalidTextureHandle;

        void Validate() const
        {
            if (!std::isfinite(BaseColor.x) || !std::isfinite(BaseColor.y) || !std::isfinite(BaseColor.z) ||
                BaseColor.x < 0.0f || BaseColor.y < 0.0f || BaseColor.z < 0.0f)
                throw std::invalid_argument("PBR material base color must be finite non-negative linear RGB.");
            if (!std::isfinite(BaseColorAlpha) || BaseColorAlpha < 0.0f || BaseColorAlpha > 1.0f)
                throw std::invalid_argument("PBR material base-color alpha must be in [0, 1].");
            if (!std::isfinite(Metallic) || Metallic < 0.0f || Metallic > 1.0f)
                throw std::invalid_argument("PBR material metallic factor must be in [0, 1].");
            if (!std::isfinite(Roughness) || Roughness < 0.04f || Roughness > 1.0f)
                throw std::invalid_argument("PBR material roughness must be in [0.04, 1].");
            if (!std::isfinite(AmbientOcclusion) || AmbientOcclusion < 0.0f || AmbientOcclusion > 1.0f)
                throw std::invalid_argument("PBR material ambient occlusion must be in [0, 1].");
            if (!std::isfinite(Emissive.x) || !std::isfinite(Emissive.y) ||
                !std::isfinite(Emissive.z) || Emissive.x < 0.0f ||
                Emissive.y < 0.0f || Emissive.z < 0.0f)
                throw std::invalid_argument("PBR material emissive factor must be finite non-negative linear RGB.");
            if (!std::isfinite(NormalScale) || NormalScale < 0.0f || NormalScale > 16.0f)
                throw std::invalid_argument("PBR material normal scale must be in [0, 16].");
            if (!std::isfinite(AlphaCutoff) || AlphaCutoff < 0.0f || AlphaCutoff > 1.0f)
                throw std::invalid_argument("PBR material alpha cutoff must be in [0, 1].");
            switch (AlphaMode)
            {
                case MaterialAlphaMode::Opaque:
                case MaterialAlphaMode::Mask:
                case MaterialAlphaMode::Blend: break;
                default: throw std::invalid_argument("PBR material alpha mode is invalid.");
            }
        }
    };
}
