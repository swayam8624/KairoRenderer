#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace kairo::renderer::detail
{
    struct InstanceRun final
    {
        std::size_t First = 0u;
        std::uint32_t Count = 1u;
    };

    // Never reorder draws. Ineligible entries (including transparent or
    // skinned draws) break runs. Only adjacent compatible opaque draws share
    // a command. Count is bounded by Vulkan's uint32 instanceCount.
    template<class Draws, class Eligible, class Compatible>
    [[nodiscard]] std::vector<InstanceRun> PlanInstanceRuns(
        const Draws& draws, Eligible eligible, Compatible compatible)
    {
        std::vector<InstanceRun> runs;
        for (std::size_t first = 0u; first < draws.size();)
        {
            std::uint32_t count = 1u;
            if (eligible(draws[first]))
                while (count < std::numeric_limits<std::uint32_t>::max() &&
                    count < draws.size() - first &&
                    eligible(draws[first + count]) &&
                    compatible(draws[first], draws[first + count]))
                    ++count;
            runs.push_back({first, count});
            first += count;
        }
        return runs;
    }

    // Stable compact index stream. A run's First refers to this stream,
    // not the submitted draw array: descriptors always use original indices.
    // Reuse storage and allow invisible/noncaster entries to disappear
    // without splitting compatible adjacent surviving draws.
    template<class Draws, class Include, class Eligible, class Compatible>
    void PlanIndexedInstanceRuns(const Draws& draws, Include include,
        Eligible eligible, Compatible compatible,
        std::vector<std::size_t>& indices, std::vector<InstanceRun>& runs)
    {
        indices.clear();
        runs.clear();
        for (std::size_t i = 0; i < draws.size(); ++i)
            if (include(i, draws[i])) indices.push_back(i);
        for (std::size_t first = 0; first < indices.size();)
        {
            std::uint32_t count = 1u;
            const auto& head = draws[indices[first]];
            if (eligible(head))
                while (count < std::numeric_limits<std::uint32_t>::max() &&
                    count < indices.size() - first &&
                    eligible(draws[indices[first + count]]) &&
                    compatible(head, draws[indices[first + count]]))
                    ++count;
            runs.push_back({first, count});
            first += count;
        }
    }
}
