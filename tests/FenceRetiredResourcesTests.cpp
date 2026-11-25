#include "../detail/FenceRetiredResources.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
struct Resource
{
    std::unique_ptr<int> Value;
    unsigned* Releases;
    Resource(int id,unsigned& releases):Value(std::make_unique<int>(id)),Releases(&releases){}
    Resource(Resource&&) noexcept=default;
    ~Resource(){if(Value)++*Releases;}
};
void Check(bool ok,const char* m){if(!ok)throw std::runtime_error(m);}
}
int main()
{
    using kairo::renderer::detail::FenceRetiredResources;
    unsigned releases=0;
    FenceRetiredResources<Resource> retired(256);
    for(unsigned frame=0;frame<400;++frame)
    {
        for(unsigned i=0;i<256;++i)
        {
            Resource r(static_cast<int>(i),releases);
            retired.Retire(std::move(r));
            Check(!r.Value,"Retirement did not transfer ownership");
            Check(releases==frame*257,"Resource released before fence");
        }
        Check(retired.Full()&&retired.Size()==256,"Capacity mismatch");
        {
            Resource extra(257,releases);
            bool rejected=false;
            try{retired.Retire(std::move(extra));}
            catch(const std::length_error&){rejected=true;}
            Check(rejected&&extra.Value&&*extra.Value==257,"Full queue consumed live resource");
        }
        Check(releases==frame*257+1,"Full queue released retired resources");
        retired.ReleaseCompleted();
        Check(retired.Size()==0&&releases==(frame+1)*257,"Fence release count mismatch");
        retired.ReleaseCompleted();
    }
    std::cout<<"102400 deferred releases, 400 capacity rejections and completion cycles passed\n";
}
