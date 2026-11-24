#pragma once

#include "VulkanFrustumBounds.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>
#include <utility>

namespace kairo::renderer::detail
{
    // Enclose the actual float shader's affine world-position product.
    // Only ordinary finite affine transforms participate; projective,
    // extreme, invalid and deforming geometry must use the individual path.
    template<class Matrix>
    [[nodiscard]] LocalBounds ConservativeWorldBounds(const LocalBounds& local,
        const Matrix& model) noexcept
    {
        if (!local.Valid || model(3,0) != 0 || model(3,1) != 0 ||
            model(3,2) != 0 || model(3,3) != 1) return {};
        constexpr double limit = 1.0e8;
        constexpr double eps = 512.0 * std::numeric_limits<float>::epsilon();
        constexpr double tiny = 512.0 * std::numeric_limits<float>::min() * limit;
        LocalBounds result;
        for (std::size_t row = 0; row < 3; ++row)
        {
            double lo = model(row,3), hi = lo, magnitude = std::abs(lo);
            if (!std::isfinite(lo) || std::abs(lo) > limit) return {};
            for (std::size_t col = 0; col < 3; ++col)
            {
                const double a = model(row,col), l = local.Min[col], u = local.Max[col];
                if (!std::isfinite(a) || !std::isfinite(l) || !std::isfinite(u) ||
                    std::abs(a) > limit || std::abs(l) > limit || std::abs(u) > limit || l > u)
                    return {};
                lo += std::min(a*l, a*u);
                hi += std::max(a*l, a*u);
                magnitude += std::abs(a)*std::max(std::abs(l),std::abs(u));
            }
            const double margin = eps*magnitude + tiny;
            result.Min[row] = lo-margin;
            result.Max[row] = hi+margin;
            if (std::abs(result.Min[row]) > limit || std::abs(result.Max[row]) > limit)
                return {};
        }
        result.Valid = true;
        return result;
    }

    class StaticVisibilityBvh final
    {
    public:
        struct Entry { std::size_t Draw; LocalBounds Bounds; };
        void Clear() noexcept { m_Entries.clear(); m_Nodes.clear(); }

        // Caller supplies only conservative, valid world bounds. Median split
        // by largest node extent; ties use the original draw index.
        void Build(std::vector<Entry> entries)
        {
            Clear();
            m_Entries = std::move(entries);
            if (!m_Entries.empty()) BuildNode(0, m_Entries.size());
        }

        // Mark complete rejected subtrees by original draw index. Surviving
        // leaves call the existing per-object test; never reorder submissions.
        template<class Outside, class Reject, class Test>
        void Cull(Outside outside, Reject reject, Test test) const
        {
            if (!m_Nodes.empty()) Visit(0, outside, reject, test);
        }

        [[nodiscard]] std::size_t Size() const noexcept { return m_Entries.size(); }

    private:
        struct Node
        {
            LocalBounds Bounds;
            std::size_t First, End, Right = 0;
        };
        std::vector<Entry> m_Entries;
        std::vector<Node> m_Nodes;

        static void Union(LocalBounds& a, const LocalBounds& b) noexcept
        {
            if (!a.Valid) { a = b; return; }
            for (std::size_t i = 0; i < 3; ++i)
            {
                a.Min[i] = std::min(a.Min[i],b.Min[i]);
                a.Max[i] = std::max(a.Max[i],b.Max[i]);
            }
        }
        static double Center(const Entry& e, std::size_t axis) noexcept
        { return (e.Bounds.Min[axis]+e.Bounds.Max[axis])*0.5; }

        void BuildNode(std::size_t first, std::size_t end)
        {
            LocalBounds bounds;
            for (std::size_t i = first; i < end; ++i) Union(bounds,m_Entries[i].Bounds);
            const std::size_t node = m_Nodes.size();
            m_Nodes.push_back({bounds,first,end,0});
            if (end-first <= 8) return;
            std::size_t axis = 0;
            for (std::size_t i = 1; i < 3; ++i)
                if (bounds.Max[i]-bounds.Min[i] > bounds.Max[axis]-bounds.Min[axis]) axis=i;
            const std::size_t mid = first+(end-first)/2;
            using Diff = std::vector<Entry>::difference_type;
            std::nth_element(m_Entries.begin()+static_cast<Diff>(first),
                m_Entries.begin()+static_cast<Diff>(mid),
                m_Entries.begin()+static_cast<Diff>(end),
                [axis](const Entry& a, const Entry& b)
                {
                    const double x=Center(a,axis), y=Center(b,axis);
                    return x == y ? a.Draw < b.Draw : x < y;
                });
            BuildNode(first,mid);
            m_Nodes[node].Right=m_Nodes.size();
            BuildNode(mid,end);
        }
        template<class Outside,class Reject,class Test>
        void Visit(std::size_t index, Outside& outside, Reject& reject, Test& test) const
        {
            const auto& node=m_Nodes[index];
            if (outside(node.Bounds))
            {
                for (std::size_t i=node.First;i<node.End;++i) reject(m_Entries[i].Draw);
            }
            else if (node.Right == 0)
            {
                for (std::size_t i=node.First;i<node.End;++i) test(m_Entries[i].Draw);
            }
            else
            {
                Visit(index+1,outside,reject,test);
                Visit(node.Right,outside,reject,test);
            }
        }
    };
}
