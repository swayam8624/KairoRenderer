#include "../detail/MetalInstanceCompatibility.hpp"
#include "../detail/InstanceRuns.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

int main()
{
    using namespace kairo::renderer::detail;
    const auto check=[](bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);};
    MetalDraw a;
    a.Mesh=1;
    std::vector<MetalDraw> draws(100000,a);
    for(std::size_t i=0;i<draws.size();++i)
    {
        draws[i].Model[3]=static_cast<float>(i);
        draws[i].Material.ObjectID=static_cast<std::uint32_t>(i);
        draws[i].Material.ReceiveShadows=i%2==0;
    }
    auto plan=[&](){return PlanInstanceRuns(draws,MetalInstanceEligible,MetalInstanceCompatible);};
    check(plan().size()==1 && plan()[0].Count==100000,"Per-instance state fragmented run");
    auto b=a;
    b.Material.BaseColor[0]=std::nextafter(0.0f,1.0f);
    check(!MetalInstanceCompatible(a,b),"Exact base color change missed");
    for(auto field:{&MetalMaterial::Metallic,&MetalMaterial::Roughness,
        &MetalMaterial::AmbientOcclusion,&MetalMaterial::NormalScale,&MetalMaterial::AlphaCutoff})
    {
        b=a;
        b.Material.*field=std::nextafter(a.Material.*field,2.0f);
        check(!MetalInstanceCompatible(a,b),"Material factor ignored");
    }
    for(auto field:{&MetalMaterial::BaseColorTexture,&MetalMaterial::NormalTexture,
        &MetalMaterial::MetallicRoughnessTexture,&MetalMaterial::EmissiveTexture,
        &MetalMaterial::OcclusionTexture})
    {
        b=a; b.Material.*field=17;
        check(!MetalInstanceCompatible(a,b),"Texture slot ignored");
    }
    b=a;b.Material.DoubleSided=true;
    check(!MetalInstanceCompatible(a,b),"Cull state ignored");
    b=a;b.Material.Emissive[2]=1;
    check(!MetalInstanceCompatible(a,b),"Emissive factor ignored");
    b=a;b.Mesh=2;
    check(!MetalInstanceCompatible(a,b),"Geometry handle ignored");
    draws[50000].Material.AlphaMode=3;
    check(plan().size()==3,"Blended draw merged");
    draws[50000].Material.AlphaMode=2;
    check(plan().size()==3,"Masked draw merged");
    draws[50000].Material.AlphaMode=1;
    draws[50000].SkinJointCount=1;
    check(plan().size()==3,"Skinned draw merged");
    draws.assign(100000,a);
    for(std::size_t i=0;i<draws.size();++i)
    {
        draws[i].CameraVisible=i%2==0;
        draws[i].CastShadows=i%3==0;
    }
    std::vector<std::size_t> indices;
    std::vector<InstanceRun> runs;
    PlanIndexedInstanceRuns(draws,MetalShadowIncluded,MetalInstanceEligible,
        MetalInstanceCompatible,indices,runs);
    check(runs.size()==1 && runs[0].Count==33334,"Camera visibility fragmented shadows");
    for(std::size_t i=0;i<indices.size();++i)
        check(indices[i]==i*3,"Shadow planning changed caster index/order");
    draws[3].Material.AlphaMode=3;
    draws[6].Material.AlphaMode=2;
    PlanIndexedInstanceRuns(draws,MetalShadowIncluded,MetalInstanceEligible,
        MetalInstanceCompatible,indices,runs);
    check(indices.size()==33333 && indices[1]==6 && runs.size()==3 &&
          runs[1].Count==1,"Blended or masked shadow fallback changed");
    std::cout<<"100000 Metal instances, 33334 independent shadow casters and fallback passed\n";
}
