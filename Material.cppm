module;

#include <cmath>
#include <stdexcept>

export module Kairo.Renderer.Material;

import Kairo.Foundation.Math;

export namespace kairo::renderer
{
    /// Compact metallic-roughness material factors for the real-time forward
    /// pass. Values are linear, not display-encoded.
    ///
    /// BaseColor: non-negative linear RGB multiplier applied to vertex color.
    /// Metallic: 0 for dielectric, 1 for conductor, blends allowed in between.
    /// Roughness: perceptual roughness in [0.04, 1]; the lower bound avoids a
    /// numerically singular microfacet distribution without hiding a delta BRDF.
    /// AmbientOcclusion: indirect-light visibility in [0, 1].
    struct PBRMaterial final
    {
        kairo::foundation::math::Vec3f BaseColor{ 1.0f, 1.0f, 1.0f };
        float Metallic = 0.0f;
        float Roughness = 0.55f;
        float AmbientOcclusion = 1.0f;

        void Validate() const
        {
            if (!std::isfinite(BaseColor.x) || !std::isfinite(BaseColor.y) || !std::isfinite(BaseColor.z) ||
                BaseColor.x < 0.0f || BaseColor.y < 0.0f || BaseColor.z < 0.0f)
                throw std::invalid_argument("PBR material base color must be finite non-negative linear RGB.");
            if (!std::isfinite(Metallic) || Metallic < 0.0f || Metallic > 1.0f)
                throw std::invalid_argument("PBR material metallic factor must be in [0, 1].");
            if (!std::isfinite(Roughness) || Roughness < 0.04f || Roughness > 1.0f)
                throw std::invalid_argument("PBR material roughness must be in [0.04, 1].");
            if (!std::isfinite(AmbientOcclusion) || AmbientOcclusion < 0.0f || AmbientOcclusion > 1.0f)
                throw std::invalid_argument("PBR material ambient occlusion must be in [0, 1].");
        }
    };
}
