module;

#include <cmath>
#include <stdexcept>

export module Kairo.Renderer.ShadowSettings;

export namespace kairo::renderer
{
    /// Directional shadow controls shared by runtime tools and the forward pass.
    ///
    /// Input: finite values within the documented ranges.
    /// Output: deterministic depth bias and receiver shadowing behavior.
    /// Task: expose safe tuning without leaking Vulkan structures into engine
    /// or editor code. Resolution is deliberately renderer-owned in M10; atlas
    /// sizing and cascades require a later render-resource policy.
    struct DirectionalShadowSettings final
    {
        bool Enabled = true;
        float Strength = 0.82f;
        float ConstantDepthBias = 1.25f;
        float SlopeDepthBias = 1.75f;
        float ReceiverBias = 0.0015f;

        void Validate() const
        {
            if (!std::isfinite(Strength) || Strength < 0.0f || Strength > 1.0f)
                throw std::invalid_argument("Directional shadow strength must be in [0, 1].");
            if (!std::isfinite(ConstantDepthBias) || ConstantDepthBias < 0.0f || ConstantDepthBias > 16.0f)
                throw std::invalid_argument("Directional shadow constant depth bias must be in [0, 16].");
            if (!std::isfinite(SlopeDepthBias) || SlopeDepthBias < 0.0f || SlopeDepthBias > 16.0f)
                throw std::invalid_argument("Directional shadow slope depth bias must be in [0, 16].");
            if (!std::isfinite(ReceiverBias) || ReceiverBias < 0.0f || ReceiverBias > 0.05f)
                throw std::invalid_argument("Directional shadow receiver bias must be in [0, 0.05].");
        }
    };
}
