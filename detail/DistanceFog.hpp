#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kairo::renderer::detail
{
    // Stable GPU encoding: 0=off, 1=exponential, 2=linear. The public
    // renderer enum has matching values; the authoring layer maps explicitly.
    enum class DistanceFogEquation:unsigned{Disabled=0,Exponential=1,Linear=2};
    inline void ValidateDistanceFog(DistanceFogEquation equation,float density,float start,float end)
    {
        if(!std::isfinite(density)||density<0.f||density>1000.f||
            !std::isfinite(start)||start<0.f||start>1e7f||
            !std::isfinite(end)||end<0.f||end>1e7f)
            throw std::invalid_argument("Distance fog parameters outside supported bounds.");
        switch(equation)
        {
            case DistanceFogEquation::Disabled:
            case DistanceFogEquation::Exponential:break;
            case DistanceFogEquation::Linear:
                if(end<=start)throw std::invalid_argument("Linear fog end must exceed start.");
                break;
            default:throw std::invalid_argument("Unknown distance fog equation.");
        }
    }
    // Compatibility overload: exponential fog has no end cutoff.
    inline void ValidateDistanceFog(float density,float start)
    {ValidateDistanceFog(DistanceFogEquation::Exponential,density,start,0.f);}
    // Reference for the native lit fragment equation, not a CPU postprocess.
    // The interval endpoints are exact and zero-density exponential is off.
    [[nodiscard]] inline float DistanceFogVisibility(DistanceFogEquation equation,float density,float start,float end,float distance)
    {
        ValidateDistanceFog(equation,density,start,end);
        if(!std::isfinite(distance)||distance<0.f)throw std::invalid_argument("Invalid fog sample distance.");
        if(equation==DistanceFogEquation::Disabled||distance<=start)return 1.f;
        if(equation==DistanceFogEquation::Linear)return std::clamp((end-distance)/(end-start),0.f,1.f);
        if(density==0.f)return 1.f;
        return std::exp(-density*(distance-start));
    }
    [[nodiscard]] inline float DistanceFogVisibility(float density,float start,float distance)
    {return DistanceFogVisibility(DistanceFogEquation::Exponential,density,start,0.f,distance);}
}
