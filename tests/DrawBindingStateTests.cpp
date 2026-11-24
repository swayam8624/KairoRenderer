#include "../detail/DrawBindingState.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
    using State = kairo::renderer::detail::DrawBindingState<unsigned, std::uint64_t>;
    struct Draw
    {
        unsigned Pipeline;
        std::uint64_t Mesh;
        unsigned Object;
        bool operator==(const Draw&) const = default;
    };
    void Check(bool ok, const char* message)
    {
        if (!ok) throw std::runtime_error(message);
    }
    struct Counts { std::size_t Pipeline = 0, Mesh = 0; };
    Counts Replay(const std::vector<Draw>& input)
    {
        State state;
        Counts counts;
        unsigned pipeline = 0u;
        std::uint64_t mesh = 0u;
        std::vector<Draw> recorded;
        for (const auto& draw : input)
        {
            state.PipelineFor(draw.Pipeline, [&] { pipeline = draw.Pipeline; ++counts.Pipeline; });
            state.MeshFor(draw.Mesh, [&] { mesh = draw.Mesh; ++counts.Mesh; });
            // Draw-local descriptor and push-constant state is never skipped.
            recorded.push_back({ pipeline, mesh, draw.Object });
        }
        Check(recorded == input, "State reuse changed draw output/order");
        std::size_t expectedPipelines = 0, expectedMeshes = 0;
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            if (i == 0 || input[i].Pipeline != input[i-1].Pipeline) ++expectedPipelines;
            if (i == 0 || input[i].Mesh != input[i-1].Mesh) ++expectedMeshes;
        }
        Check(counts.Pipeline == expectedPipelines && counts.Mesh == expectedMeshes,
            "Binding count differs from exact run count");
        return counts;
    }
}
int main()
{
    const auto empty = Replay({});
    Check(empty.Pipeline == 0 && empty.Mesh == 0, "Empty pass bound state");
    std::vector<Draw> repeated;
    for (unsigned i = 0; i < 100000; ++i) repeated.push_back({1u, 42u, i});
    const auto counts = Replay(repeated);
    Check(counts.Pipeline == 1 && counts.Mesh == 1, "Repeated draws rebound state");
    // Fresh pass: even identical handles must bind again.
    const auto next = Replay(repeated);
    Check(next.Pipeline == 1 && next.Mesh == 1, "State leaked across passes");
    std::mt19937 rng(82601u);
    for (unsigned trial = 0; trial < 500; ++trial)
    {
        std::vector<Draw> draws;
        for (unsigned i = 0; i < 512; ++i)
            draws.push_back({static_cast<unsigned>(rng() % 4u),
                static_cast<std::uint64_t>(rng() % 8u), i});
        (void)Replay(draws);
    }
    State state;
    bool failed = false;
    try { state.PipelineFor(7u, [] { throw std::runtime_error("injected"); }); }
    catch (const std::runtime_error&) { failed = true; }
    unsigned retries = 0u;
    state.PipelineFor(7u, [&] { ++retries; });
    state.PipelineFor(7u, [&] { ++retries; });
    Check(failed && retries == 1u, "Failed bind published state");
    std::cout << "100000 repeated draws: 1 pipeline bind, 1 mesh bind pair; "
                 "256000 randomized draws, fresh passes and failure retry passed\n";
}
