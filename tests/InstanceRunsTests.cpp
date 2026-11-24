#include "../detail/InstanceRuns.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
    struct Draw
    {
        unsigned Mesh = 0, Material = 0, ID = 0;
        bool Opaque = true, Skinned = false;
    };
    bool Eligible(const Draw& d) { return d.Opaque && !d.Skinned; }
    bool Compatible(const Draw& a, const Draw& b)
    {
        return a.Mesh == b.Mesh && a.Material == b.Material;
    }
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    std::size_t Verify(const std::vector<Draw>& input)
    {
        const auto runs = kairo::renderer::detail::PlanInstanceRuns(input, Eligible, Compatible);
        std::size_t next = 0;
        for (const auto& run : runs)
        {
            Check(run.First == next && run.Count > 0 && run.Count <= input.size() - next,
                "Run dropped, reordered or duplicated a draw");
            for (std::size_t i = 0; i < run.Count; ++i)
                if (run.Count > 1)
                    Check(Eligible(input[next + i]) && Compatible(input[next], input[next + i]),
                        "Incompatible instance included");
            next += run.Count;
            if (next < input.size() && Eligible(input[run.First]) && Eligible(input[next]))
                Check(!Compatible(input[run.First], input[next]), "Compatible run split");
        }
        Check(next == input.size(), "Run list omitted tail");
        return runs.size();
    }
}
int main()
{
    Check(Verify({}) == 0, "Empty draw list produced runs");
    std::vector<Draw> repeated(100000);
    for (unsigned i = 0; i < repeated.size(); ++i) repeated[i].ID = i;
    Check(Verify(repeated) == 1, "Identical opaque meshes failed to merge");
    repeated[50000].Skinned = true;
    Check(Verify(repeated) == 3, "Skinned draw failed to break run");
    repeated[50000].Skinned = false;
    repeated[50000].Opaque = false;
    Check(Verify(repeated) == 3, "Transparent/masked draw failed to break run");
    repeated[50000].Opaque = true;
    repeated[50000].Material = 5;
    Check(Verify(repeated) == 3, "Material boundary failed to break run");
    std::mt19937 rng(82602u);
    for (unsigned trial = 0; trial < 500; ++trial)
    {
        std::vector<Draw> draws;
        for (unsigned i = 0; i < 512; ++i)
            draws.push_back({static_cast<unsigned>(rng() % 3u),
                static_cast<unsigned>(rng() % 3u), i, rng() % 5u != 0u, rng() % 7u == 0u});
        (void)Verify(draws);
    }
    std::cout << "100000 compatible draws -> 1 instance run; 256000 randomized draws and fallback boundaries passed\n";
}
