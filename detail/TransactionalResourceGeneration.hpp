#pragma once

// CPU-only transactional publication used by VulkanMaterialDescriptors.
// GPU memory is owned by the caller's Generation type, not this helper.
// Candidate and live generations must be independent and the caller must have
// retired GPU commands referring to the old active generation before commit.
//
// Key exception-safety rule:
//   1. Build ALL candidate resources without mutating active.
//   2. If any validation, allocation, or upload throws, staged's destructor
//      rolls back the candidate and active remains exactly unchanged.
//   3. Publish by a statically guaranteed noexcept Swap.
//   4. The caller's staged object then owns and retires the former generation.
//
// Unlike clearing the old descriptor pool before allocation, this provides
// the STRONG guarantee for host exceptions, subject to the native backend's
// Create/Destroy operations honoring their documented ownership contracts.
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace kairo::renderer::detail
{
    template<class Generation, class Build>
    void BuildAndCommitResourceGeneration(
        Generation& active, Generation& staged, Build&& build)
    {
        static_assert(noexcept(std::declval<Generation&>().Swap(
            std::declval<Generation&>())),
            "Publishing a resource generation must never throw.");
        if (std::addressof(active) == std::addressof(staged))
            throw std::invalid_argument(
                "Cannot stage a resource generation inside the active owner.");

        // The Build callback is only allowed to allocate/populate staged.
        // It must not destroy or mutate active; this is the strong-guarantee
        // boundary. The active generation is untouched on any thrown error.
        std::invoke(std::forward<Build>(build), staged);

        // Atomic at the host-object ownership level, no allocation/throw.
        // GPU fence retirement is an independent requirement of the caller.
        active.Swap(staged);
    }
}
