#pragma once

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace kairo::renderer::detail
{
    // A bounded journal of native allocations detached from public handles.
    // Only clear after the owning backend proves completion of ALL work
    // that could reference these resources. Destruction of the journal
    // itself requires the same lifetime guarantee.
    template<class Resource>
    class FenceRetiredResources final
    {
        static_assert(std::is_nothrow_move_constructible_v<Resource>);
    public:
        explicit FenceRetiredResources(std::size_t capacity)
        {
            if(capacity==0)throw std::invalid_argument("Retirement capacity must be positive.");
            m_Resources.reserve(capacity);
            m_Limit=capacity;
        }

        [[nodiscard]] bool Full() const noexcept {return Size()==m_Limit;}
        [[nodiscard]] std::size_t Size() const noexcept {return m_Resources.size();}

        // Check capacity BEFORE moving from the caller's live resource.
        // The fixed reserved storage makes publication allocation-free.
        void Retire(Resource&& resource)
        {
            if(Full())throw std::length_error("Fence retirement journal is full.");
            m_Resources.emplace_back(std::move(resource));
        }

        void ReleaseCompleted() noexcept {m_Resources.clear();}

    private:
        std::vector<Resource> m_Resources;
        std::size_t m_Limit=0;
    };
}
