#include "EmbeddedVulkanShaders.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc,char** argv)
{
    using kairo::renderer::detail::EmbeddedVulkanShader;
    if(argc!=2)throw std::invalid_argument("Expected compiled shader directory.");
    constexpr std::array<std::string_view,12> names{
        "viewport_present.vert.spv","viewport_present.frag.spv",
        "instanced_shadow.vert.spv","instanced_triangle.vert.spv","instanced_triangle.frag.spv",
        "triangle.vert.spv","skinned_triangle.vert.spv","triangle.frag.spv",
        "debug_line.vert.spv","debug_line.frag.spv","shadow.vert.spv","skinned_shadow.vert.spv"};
    std::size_t words=0;
    for(const auto name:names)
    {
        const auto embedded=EmbeddedVulkanShader(name);
        if(embedded.size()<5||embedded.front()!=0x07230203u)throw std::runtime_error("Embedded shader header differs.");
        std::ifstream file(std::filesystem::path(argv[1])/name,std::ios::binary);
        if(!file)throw std::runtime_error("Reference shader missing.");
        for(auto word:embedded)
        {
            std::array<unsigned char,4> bytes{};
            if(!file.read(reinterpret_cast<char*>(bytes.data()),4))throw std::runtime_error("Reference shader truncated.");
            const auto expected=std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8)|
                (std::uint32_t(bytes[2])<<16)|(std::uint32_t(bytes[3])<<24);
            if(word!=expected)throw std::runtime_error("Embedded shader word differs.");
            ++words;
        }
        if(file.peek()!=std::char_traits<char>::eof()||file.bad())throw std::runtime_error("Embedded shader length differs.");
    }
    bool rejected=false;
    try{(void)EmbeddedVulkanShader("missing.spv");}catch(const std::invalid_argument&){rejected=true;}
    if(!rejected)throw std::runtime_error("Unknown shader accepted.");
    std::cout<<names.size()<<" embedded shaders, "<<words<<" exact SPIR-V words and unknown-name rejection passed\n";
}
