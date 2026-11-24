#include "../detail/MetalSkinUpload.hpp"
#include <iostream>
#include <stdexcept>

int main()
{
    using namespace kairo::renderer::detail;
    std::size_t total=0;
    for (std::size_t i=0;i<100000;++i)
    {
        const auto joints=i%256;
        const auto before=total;
        const auto range=AppendMetalSkinRange(total,joints);
        if (joints==0)
        {
            if (range.Bytes!=0 || total!=before) throw std::runtime_error("Empty palette allocated storage");
        }
        else if (range.Offset%MetalSkinAlignment!=0 || range.Offset<before ||
                 range.Bytes!=joints*64 || total!=range.Offset+range.Bytes)
            throw std::runtime_error("Skin range overlapped or lost alignment");
    }
    bool rejected=false;
    try { (void)AppendMetalSkinRange(total,256); }
    catch(const std::length_error&) { rejected=true; }
    if(!rejected) throw std::runtime_error("Unsupported joint count accepted");
    total=std::numeric_limits<std::size_t>::max();
    rejected=false;
    try { (void)AppendMetalSkinRange(total,1); }
    catch(const std::length_error&) { rejected=true; }
    if(!rejected) throw std::runtime_error("Overflow accepted");
    std::cout << "100000 palette ranges, 255-joint limit and overflow passed\n";
}
