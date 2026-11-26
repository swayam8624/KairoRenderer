#include "../detail/DistanceFog.hpp"
#include <iostream>
#include <limits>
#include <random>
using namespace kairo::renderer::detail;
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main()
{
    Check(DistanceFogVisibility(0.f,0.f,1e6f)==1.f,"disabled fog changed visibility");
    Check(DistanceFogVisibility(1.f,4.f,3.f)==1.f&&DistanceFogVisibility(1.f,4.f,4.f)==1.f,"fog before start");
    Check(std::abs(DistanceFogVisibility(.5f,1.f,3.f)-std::exp(-1.f))<1e-7f,"known optical depth");
    std::mt19937 rng(631);
    std::uniform_real_distribution<float> density(0.f,2.f),start(0.f,100.f),delta(0.f,100.f);
    for(unsigned i=0;i<50000;++i)
    {
        const float d=density(rng),s=start(rng),x=s+delta(rng),y=x+delta(rng);
        const auto a=DistanceFogVisibility(d,s,x),b=DistanceFogVisibility(d,s,y);
        const double oracle=std::exp(-static_cast<double>(d)*(static_cast<double>(x)-s));
        Check(a>=0.f&&a<=1.f&&b<=a&&b>=0.f,"visibility bounds or monotonicity");
        Check(std::abs(static_cast<double>(a)-oracle)<2e-7,"double precision reference mismatch");
        Check(DistanceFogVisibility(d,s,s)==1.f,"start discontinuity");
    }
    for(float invalid:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
    {
        bool rejected=false;try{ValidateDistanceFog(invalid,0.f);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"invalid density");
        rejected=false;try{ValidateDistanceFog(0.f,invalid);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"invalid start");
        rejected=false;try{(void)DistanceFogVisibility(1.f,0.f,invalid);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"invalid distance");
    }
    bool rejected=false;try{ValidateDistanceFog(1001.f,0.f);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"density cap");
    rejected=false;try{ValidateDistanceFog(1.f,1e7f);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"start cap");
    std::cout<<"50000 distance fog samples matched double reference; bounds, monotonicity and disabled behavior passed\n";
}
