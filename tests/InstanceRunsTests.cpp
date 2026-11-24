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
        bool CameraVisible = true, CastShadows = true;
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
    // Camera culling must not affect the light's independent plan.
    std::vector<Draw> casters(1000);
    casters[400].CameraVisible = false;
    const auto main = kairo::renderer::detail::PlanInstanceRuns(casters,
        [](const Draw& d) { return Eligible(d) && d.CameraVisible; }, Compatible);
    const auto shadow = kairo::renderer::detail::PlanInstanceRuns(casters,
        [](const Draw& d) { return Eligible(d) && d.CastShadows; }, Compatible);
    Check(main.size() == 3 && shadow.size() == 1 && shadow[0].Count == 1000,
        "Camera visibility incorrectly constrained shadow plan");
    casters[500].CastShadows = false;
    const auto split = kairo::renderer::detail::PlanInstanceRuns(casters,
        [](const Draw& d) { return Eligible(d) && d.CastShadows; }, Compatible);
    Check(split.size() == 3 && split[1].Count == 1 && split[1].First == 500,
        "Noncaster failed to split shadow run");
    // A filtered draw must not occupy a descriptor slot in the run.
    // Original draw indices survive compaction, including holes in the list.
    std::vector<std::size_t> indices;
    std::vector<kairo::renderer::detail::InstanceRun> runs;
    std::vector<Draw> sparse(100000);
    for (std::size_t i = 0; i < sparse.size(); ++i)
    {
        sparse[i].CameraVisible = i % 2 == 0;
        sparse[i].CastShadows = i % 3 == 0;
    }
    const auto plan = [&](bool shadowPass, const std::vector<Draw>& draws)
    {
        kairo::renderer::detail::PlanIndexedInstanceRuns(draws,
            [=](std::size_t, const Draw& d) { return shadowPass ? d.CastShadows : d.CameraVisible; },
            Eligible, Compatible, indices, runs);
        std::vector<std::size_t> expected;
        for (std::size_t i = 0; i < draws.size(); ++i)
            if (shadowPass ? draws[i].CastShadows : draws[i].CameraVisible) expected.push_back(i);
        Check(indices == expected, "Compaction reordered or omitted surviving draw IDs");
        std::size_t next = 0;
        for (const auto& run : runs)
        {
            Check(run.First == next && run.Count > 0 && run.Count <= indices.size() - next,
                "Indexed run coverage differs");
            for (std::size_t i = 0; i < run.Count; ++i)
                if (run.Count > 1)
                    Check(Eligible(draws[indices[next + i]]) &&
                        Compatible(draws[indices[next]], draws[indices[next + i]]),
                        "Indexed run crossed incompatible geometry or material");
            next += run.Count;
        }
        Check(next == indices.size(), "Indexed run omitted tail");
    };
    plan(false, sparse);
    Check(runs.size() == 1 && runs[0].Count == 50000u, "Invisible gaps fragmented main batch");
    const auto capacity = indices.capacity();
    plan(true, sparse);
    Check(runs.size() == 1 && runs[0].Count == 33334u, "Noncaster gaps fragmented shadow batch");
    Check(indices.capacity() == capacity, "Compact index capacity not reused");
    plan(false, {});
    Check(runs.empty() && indices.empty(), "Empty replan retained stale draws");
    std::mt19937 rng(82602u);
    for (unsigned trial = 0; trial < 500; ++trial)
    {
        std::vector<Draw> draws;
        for (unsigned i = 0; i < 512; ++i)
            draws.push_back({static_cast<unsigned>(rng() % 3u),
                static_cast<unsigned>(rng() % 3u), i, rng() % 5u != 0u, rng() % 7u == 0u});
        (void)Verify(draws);
        for (auto& draw : draws)
        {
            draw.CameraVisible = rng() % 3u != 0u;
            draw.CastShadows = rng() % 2u != 0u;
        }
        plan(false, draws);
        plan(true, draws);
    }
    std::cout << "100000 compatible draws -> 1 instance run; 256000 randomized draws, fallback boundaries, independent shadow plans, and compact index streams passed\n";
}
