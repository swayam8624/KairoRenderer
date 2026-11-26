#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kairo::renderer::detail
{
    // Density is inverse world units; start and distance are world units.
    // Zero density is exactly disabled. Bound the authored domain so both
    // GPU uniforms remain finite and ordinary scene distances are usable.
    inline void ValidateDistanceFog(float density,float start)
    {
        if(!std::isfinite(density)||density<0.f||density>1000.f||
            !std::isfinite(start)||start<0.f||start>1e6f)
            throw std::invalid_argument("Distance fog density or start outside supported bounds.");
    }
    // CPU reference for the native fragment equation. This is not a CPU
    // postprocess: the renderer evaluates the same Beer-Lambert visibility
    // on world-position interpolants in the lit fragment shader.
    [[nodiscard]] inline float DistanceFogVisibility(float density,float start,float distance)
    {
        ValidateDistanceFog(density,start);
        if(!std::isfinite(distance)||distance<0.f)throw std::invalid_argument("Invalid fog sample distance.");
        if(density==0.f||distance<=start)return 1.f;
        return std::exp(-density*(distance-start));
    }
}
