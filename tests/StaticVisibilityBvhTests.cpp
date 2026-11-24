#include "../detail/StaticVisibilityBvh.hpp"
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
    using namespace kairo::renderer::detail;
    struct Point { float x,y,z; };
    struct Matrix
    {
        std::array<float,16> Data{};
        float operator()(std::size_t r,std::size_t c) const { return Data[r*4+c]; }
        static Matrix Identity()
        {
            Matrix m;
            for (std::size_t i=0;i<4;++i) m.Data[i*5]=1;
            return m;
        }
    };
    void Check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
}

int main()
{
    LocalBounds local;
    local.Include(Point{-.25f,-.25f,.25f});
    local.Include(Point{.25f,.25f,.75f});
    const auto identity=Matrix::Identity();
    const PreparedVulkanFrustum frustum(identity,identity);
    std::vector<Matrix> models(65536,identity);
    std::vector<StaticVisibilityBvh::Entry> entries;
    for (std::size_t i=0;i<models.size();++i)
    {
        // Deliberately interleave visible and distant spatial clusters.
        auto& m=models[i];
        m.Data[3]=i%16==0 ? 0.f : 20.f+static_cast<float>(i%1024);
        const auto bounds=ConservativeWorldBounds(local,m);
        Check(bounds.Valid,"Ordinary affine bounds rejected");
        entries.push_back({i,bounds});
    }
    StaticVisibilityBvh tree;
    tree.Build(std::move(entries));
    std::vector<unsigned> visited(models.size(),0);
    std::size_t nodeTests=0,leafTests=0,rejected=0;
    tree.Cull([&](const LocalBounds& b){++nodeTests;return frustum.Outside(b,identity);},
        [&](std::size_t i){++visited[i];++rejected;
            Check(frustum.Outside(local,models[i]),"Tree rejected visible cluster member");},
        [&](std::size_t i){++visited[i];++leafTests;});
    for (unsigned count:visited) Check(count==1,"Tree dropped or duplicated index");
    Check(rejected==61440 && leafTests==4096,"Cluster partition incorrect");
    Check(nodeTests+leafTests<models.size()/4,"Tree failed to reduce individual tests");

    std::mt19937 rng(82801u);
    std::uniform_real_distribution<float> random(-20.f,20.f),unit(0.f,1.f);
    std::size_t samples=0;
    for (unsigned trial=0;trial<10000;++trial)
    {
        auto m=identity;
        for (unsigned k=0;k<12;++k) m.Data[k]=random(rng);
        const auto bounds=ConservativeWorldBounds(local,m);
        Check(bounds.Valid,"Random ordinary bounds invalid");
        for (unsigned j=0;j<32;++j)
        {
            std::array<float,4> p{};
            for (unsigned a=0;a<3;++a)
                p[a]=static_cast<float>(local.Min[a]+unit(rng)*(local.Max[a]-local.Min[a]));
            p[3]=1;
            for (unsigned row=0;row<3;++row)
            {
                float v=0;
                for (unsigned col=0;col<4;++col) v+=m(row,col)*p[col];
                Check(v>=bounds.Min[row] && v<=bounds.Max[row],"World envelope missed float point");
            }
            ++samples;
        }
    }
    auto projective=identity;
    projective.Data[12]=.1f;
    Check(!ConservativeWorldBounds(local,projective).Valid,"Projective transform accepted");
    auto extreme=identity;
    extreme.Data[3]=1.0e20f;
    Check(!ConservativeWorldBounds(local,extreme).Valid,"Extreme transform accepted");
    tree.Build({});
    Check(tree.Size()==0,"Empty rebuild retained stale entries");
    tree.Cull([](const LocalBounds&){return false;},
        [](std::size_t){throw std::runtime_error("Stale rejection");},
        [](std::size_t){throw std::runtime_error("Stale leaf");});
    std::cout << "65536 objects; " << rejected << " subtree rejects; " << nodeTests
        << " node tests; " << leafTests << " individual tests; " << samples
        << " world envelope samples passed\n";
}
