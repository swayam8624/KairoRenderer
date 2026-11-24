#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <initializer_list>

namespace kairo::renderer::detail
{
    struct LocalBounds final
    {
        std::array<double, 3> Min{}, Max{};
        bool Valid = false;
        template<class Position>
        void Include(const Position& p) noexcept
        {
            const std::array<double, 3> v{p.x, p.y, p.z};
            if (!Valid) { Min = Max = v; Valid = true; return; }
            for (std::size_t i = 0; i < 3; ++i)
            {
                Min[i] = std::min(Min[i], v[i]);
                Max[i] = std::max(Max[i], v[i]);
            }
        }
    };

    // Conservative homogeneous clip test, Vulkan depth [0,w]. Transform all
    // eight local-box corners; a linear clip halfspace can reject the entire
    // box only when every corner lies strictly outside that same halfspace.
    // Do not divide by w, including for geometry behind/intersecting the eye.
    // Carry absolute expression magnitudes through the shader's three matrix
    // products to leave a generous float rounding margin, including cancellation.
    // Ambiguous, nonfinite or potentially overflowing inputs stay visible.
    template<class Matrix>
    [[nodiscard]] bool OutsideVulkanFrustum(const LocalBounds& box,
        const Matrix& model, const Matrix& view, const Matrix& projection) noexcept
    {
        if (!box.Valid) return false;
        std::array<bool, 6> outside{true,true,true,true,true,true};
        constexpr double safe = static_cast<double>(std::numeric_limits<float>::max()) / 16.0;
        constexpr double eps = 128.0 * std::numeric_limits<float>::epsilon();
        constexpr double tiny = 128.0 * std::numeric_limits<float>::min();
        // GLSL may associate projection * view before the vector product.
        // Reject no object if that intermediate matrix could overflow.
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t col = 0; col < 4; ++col)
            {
                double magnitude = 0.0;
                for (std::size_t k = 0; k < 4; ++k)
                    magnitude += std::abs(static_cast<double>(projection(row, k))) *
                        std::abs(static_cast<double>(view(k, col)));
                if (!std::isfinite(magnitude) || magnitude > safe) return false;
            }
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            std::array<double, 4> value{}, magnitude{};
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                value[axis] = (corner & (1u << axis)) ? box.Max[axis] : box.Min[axis];
                magnitude[axis] = std::abs(value[axis]) + tiny / eps;
            }
            value[3] = magnitude[3] = 1.0;
            for (const Matrix* matrix : {&model, &view, &projection})
            {
                std::array<double, 4> next{}, bound{};
                for (std::size_t row = 0; row < 4; ++row)
                    for (std::size_t col = 0; col < 4; ++col)
                    {
                        const double coefficient = (*matrix)(row, col);
                        next[row] += coefficient * value[col];
                        bound[row] += std::abs(coefficient) * magnitude[col];
                    }
                for (std::size_t i = 0; i < 4; ++i)
                    if (!std::isfinite(next[i]) || !std::isfinite(bound[i]) || bound[i] > safe)
                        return false;
                value = next;
                // Propagate a conservative allowance for flush-to-zero at
                // intermediate stages, rather than only at final clip space.
                for (auto& component : bound) component += tiny / eps;
                magnitude = bound;
            }
            const auto reject = [&](std::size_t axis, double sign, bool withW)
            {
                const double distance = sign * value[axis] + (withW ? value[3] : 0.0);
                const double tolerance = eps * (magnitude[axis] + (withW ? magnitude[3] : 0.0)) + tiny;
                return distance < -tolerance;
            };
            outside[0] = outside[0] && reject(0,  1.0, true);
            outside[1] = outside[1] && reject(0, -1.0, true);
            outside[2] = outside[2] && reject(1,  1.0, true);
            outside[3] = outside[3] && reject(1, -1.0, true);
            outside[4] = outside[4] && reject(2,  1.0, false);
            outside[5] = outside[5] && reject(2, -1.0, true);
            if (std::none_of(outside.begin(), outside.end(), [](bool x) { return x; }))
                return false;
        }
        return std::any_of(outside.begin(), outside.end(), [](bool x) { return x; });
    }
}
