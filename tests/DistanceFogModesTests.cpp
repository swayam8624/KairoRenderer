#include "../detail/DistanceFog.hpp"
#include <iostream>
#include <limits>
#include <random>
using namespace kairo::renderer::detail;
using Eq=DistanceFogEquation;
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main()
{
    Check(DistanceFogVisibility(Eq::Disabled,1000,0,1,100)==1.f,"disabled mode");
    Check(DistanceFogVisibility(Eq::Linear,0,10,20,10)==1.f,"near endpoint");
    Check(DistanceFogVisibility(Eq::Linear,0,10,20,20)==0.f,"far endpoint");
    Check(DistanceFogVisibility(Eq::Linear,0,10,20,15)==.5f,"linear midpoint");
    Check(DistanceFogVisibility(Eq::Exponential,.5f,1,0,3)==DistanceFogVisibility(.5f,1,3),"legacy equation");
    std::mt19937 rng(242);std::uniform_real_distribution<float> start(0,100000),span(.1f,100000),ratio(-1,2);
    for(unsigned i=0;i<50000;++i)
    {
        const float near=start(rng),far=near+span(rng);
        const float a=std::max(0.f,near+ratio(rng)*(far-near)),b=a+span(rng);
        const float va=DistanceFogVisibility(Eq::Linear,0,near,far,a),vb=DistanceFogVisibility(Eq::Linear,1000,near,far,b);
        const double oracle=std::clamp((double(far)-a)/(double(far)-near),0.,1.);
        Check(std::abs(double(va)-oracle)<2e-7&&va>=vb&&va<=1&&vb>=0,"linear visibility oracle/monotonicity");
        Check(DistanceFogVisibility(Eq::Linear,0,near,far,near)==1&&DistanceFogVisibility(Eq::Linear,0,near,far,far)==0,"boundary discontinuity");
    }
    const auto rejects=[](Eq eq,float d,float n,float f){bool caught=false;try{ValidateDistanceFog(eq,d,n,f);}catch(const std::invalid_argument&){caught=true;}Check(caught,"invalid fog accepted");};
    rejects(Eq::Linear,0,2,2);rejects(Eq::Linear,0,2,1);rejects(Eq::Linear,0,0,std::numeric_limits<float>::infinity());
    rejects(static_cast<Eq>(9),0,0,1);rejects(Eq::Disabled,0,0,std::numeric_limits<float>::quiet_NaN());
    Check(DistanceFogVisibility(Eq::Linear,0,9999999,10000000,10000000)==0,"maximum authored range");
    std::cout<<"50000 linear fog oracle samples, explicit disable and legacy exponential compatibility passed\n";
}
