#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

import Kairo.Renderer;

using namespace kairo::renderer;

TEST_CASE("render graph aliases long transient chains under bounded physical capacity",
    "[KairoRenderer][RenderGraph][Scale]")
{
    constexpr std::size_t passCount = 256u;
    constexpr std::uint64_t bytesPerResource = 4096u;

    RenderGraph graph;
    std::vector<RenderResourceHandle> resources;
    resources.reserve(passCount);
    for (std::size_t index = 0u; index < passCount; ++index)
        resources.push_back(graph.AddResource({
            "Transient-" + std::to_string(index),
            RenderResourceKind::Texture,
            bytesPerResource,
            true }));

    (void)graph.AddPass("Pass-0", {
        { resources[0], RenderAccessMode::Write,
            RenderResourceState::ColorAttachment }
    });

    for (std::size_t index = 1u; index < passCount; ++index)
        (void)graph.AddPass("Pass-" + std::to_string(index), {
            { resources[index - 1u], RenderAccessMode::Read,
                RenderResourceState::ShaderRead },
            { resources[index], RenderAccessMode::Write,
                RenderResourceState::ColorAttachment }
        });

    const auto compiled = graph.Compile();
    REQUIRE(compiled.PassCount() == passCount);
    REQUIRE(compiled.ResourceCount() == passCount);

    // Every resource overlaps only its immediate neighbour. The graph compiler
    // should therefore recycle a tiny number of physical slots instead of
    // allocating one native target per logical resource.
    CHECK(compiled.AliasSlots().size() <= 2u);
    CHECK(compiled.TransientAllocationBytes() <= 2u * bytesPerResource);

    const auto profile = compiled.Execute();
    REQUIRE(profile.Passes.size() == passCount);
}
