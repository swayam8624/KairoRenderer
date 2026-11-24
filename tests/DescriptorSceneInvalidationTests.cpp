#include "../detail/DescriptorSceneInvalidation.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    struct Vec { float x = 1, y = 1, z = 1; };
    struct Material
    {
        Vec BaseColor, Emissive;
        float BaseColorAlpha = 1, NormalScale = 1, Metallic = 0, Roughness = .5f,
            AmbientOcclusion = 1, AlphaCutoff = .5f;
        unsigned AlphaMode = 0;
        bool DoubleSided = false;
        unsigned BaseColorTexture = 0, NormalTexture = 0, MetallicRoughnessTexture = 0,
            EmissiveTexture = 0, OcclusionTexture = 0;
    };
    struct Matrix
    {
        std::array<float, 16> values{};
        float operator()(std::size_t r, std::size_t c) const { return values[r * 4 + c]; }
    };
    struct Palette
    {
        std::vector<Matrix> JointMatrices;
        std::size_t Size() const { return JointMatrices.size(); }
    };
    struct Draw
    {
        ::Material Material;
        Palette Skinning;
        Matrix Model;
        unsigned ObjectID = 0, Mesh = 1;
        bool CastShadows = true, ReceiveShadows = true;
    };
    void Check(bool ok, const char* message)
    {
        if (!ok) throw std::runtime_error(message);
    }
}

int main()
{
    using namespace kairo::renderer::detail;
    std::vector<Draw> original(3);
    original[0].Material.BaseColorTexture = 1;
    original[1].Material.BaseColorTexture = 2;
    original[2].Skinning.JointMatrices.resize(2);
    auto next = original;
    for (unsigned frame = 0; frame < 10000; ++frame)
    {
        next[0].Model.values[12] = static_cast<float>(frame);
        next[1].ObjectID = frame;
        next[1].CastShadows = frame % 2 != 0;
        next[2].ReceiveShadows = frame % 2 == 0;
        next[0].Mesh = frame + 1;
        Check(!DescriptorInputsChanged(original, next, 7u, 7u), "Unrelated draw state invalidated descriptors");
    }
    auto mutation = [&](auto modify)
    {
        auto changed = original;
        modify(changed);
        Check(DescriptorInputsChanged(original, changed, 7u, 7u), "Descriptor mutation missed");
    };
    for (auto member : { &Material::BaseColorAlpha, &Material::NormalScale, &Material::Metallic,
            &Material::Roughness, &Material::AmbientOcclusion, &Material::AlphaCutoff })
        mutation([&](auto& d) { d[0].Material.*member = std::nextafter(d[0].Material.*member, 2.0f); });
    for (auto vector : { &Material::BaseColor, &Material::Emissive })
        for (auto component : { &Vec::x, &Vec::y, &Vec::z })
            mutation([&](auto& d) { (d[0].Material.*vector).*component = 0.5f; });
    for (auto member : { &Material::BaseColorTexture, &Material::NormalTexture,
            &Material::MetallicRoughnessTexture, &Material::EmissiveTexture,
            &Material::OcclusionTexture, &Material::AlphaMode })
        mutation([&](auto& d) { d[0].Material.*member = 99u; });
    mutation([](auto& d) { d[0].Material.DoubleSided = true; });
    for (std::size_t joint = 0; joint < 2; ++joint)
        for (std::size_t entry = 0; entry < 16; ++entry)
            mutation([&](auto& d) { d[2].Skinning.JointMatrices[joint].values[entry] = 0.00001f; });
    mutation([](auto& d) { d[2].Skinning.JointMatrices.clear(); });
    mutation([](auto& d) { d.erase(d.begin() + 1); });
    mutation([](auto& d) { std::swap(d[0], d[1]); });
    Check(DescriptorInputsChanged(original, original, 7u, 8u), "Environment replacement missed");

    bool dirty = false;
    InvalidateDescriptorInputs(dirty, original, original, 7u, 7u);
    Check(!dirty, "Unchanged scene marked dirty");
    InvalidateDescriptorInputs(dirty, original, original, 7u, 8u);
    Check(dirty, "Changed scene remained clean");
    InvalidateDescriptorInputs(dirty, original, original, 8u, 8u);
    Check(dirty, "Pending rebuild lost after repeated submission");
    std::cout << "10000 transform-only submissions, material/palette mutation, removal, ordering and dirty latch passed\n";
}
