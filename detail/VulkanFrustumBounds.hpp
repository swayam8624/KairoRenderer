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


    // Prepared camera halfspaces: each plane is a row of P*V, or w +/- row.
    // For a local AABB, the maximum of a linear form is attained by choosing
    // one endpoint per axis. This avoids transforming eight corners per draw.
    // The old corner oracle remains available for tests and extreme inputs.
    template<class Matrix>
    class PreparedVulkanFrustum final
    {
        using Row = std::array<double, 4>;
        using Rows = std::array<Row, 4>;
        static constexpr double Limit = 1.0e8;
        static constexpr double Eps = 512.0 * std::numeric_limits<float>::epsilon();
        static constexpr double Tiny = 512.0 * std::numeric_limits<float>::min();

    public:
        PreparedVulkanFrustum(const Matrix& view, const Matrix& projection) noexcept
        {
            for (std::size_t r = 0; r < 4; ++r)
                for (std::size_t c = 0; c < 4; ++c)
                {
                    if (!Safe(view(r,c)) || !Safe(projection(r,c))) m_Valid = false;
                    for (std::size_t k = 0; k < 4; ++k)
                    {
                        const double a = projection(r,k), b = view(k,c);
                        m_Camera[r][c] += a*b;
                        m_AbsCamera[r][c] += std::abs(a)*std::abs(b);
                    }
                }
        }

        [[nodiscard]] bool Outside(const LocalBounds& box, const Matrix& model) const noexcept
        {
            if (!box.Valid) return false;
            // Fail open for extreme numeric inputs. The reference remains
            // useful for validation; no rejection is worth unsafe arithmetic.
            if (!m_Valid) return false;
            Row coordinateMagnitude{0,0,0,1};
            for (std::size_t a = 0; a < 3; ++a)
            {
                if (!Safe(box.Min[a]) || !Safe(box.Max[a]) || box.Min[a] > box.Max[a])
                    return false;
                coordinateMagnitude[a] = std::max(std::abs(box.Min[a]), std::abs(box.Max[a]));
            }
            Row modelMagnitude{};
            for (std::size_t r = 0; r < 4; ++r)
                for (std::size_t c = 0; c < 4; ++c)
                {
                    if (!Safe(model(r,c))) return false;
                    modelMagnitude[r] += std::abs(static_cast<double>(model(r,c))) * coordinateMagnitude[c];
                }
            Row clipMagnitude{};
            for (std::size_t r = 0; r < 4; ++r)
                for (std::size_t k = 0; k < 4; ++k)
                    clipMagnitude[r] += m_AbsCamera[r][k] * modelMagnitude[k];

            // Bound rounding/cancellation by the absolute product chain.
            // Limit bounds all three matrix products far below float max.
            // Tiny * Limit^3 also covers flushed subnormals in intermediates.
            constexpr double underflowMargin = Tiny * 256.0 * Limit * Limit * Limit;
            const auto reject = [&](std::size_t axis, double sign, bool withW)
            {
                Row local{};
                for (std::size_t col = 0; col < 4; ++col)
                    for (std::size_t k = 0; k < 4; ++k)
                        local[col] += (sign * m_Camera[axis][k] +
                            (withW ? m_Camera[3][k] : 0.0)) * model(k,col);
                double maximum = local[3];
                for (std::size_t a = 0; a < 3; ++a)
                    maximum += local[a] * (local[a] >= 0.0 ? box.Max[a] : box.Min[a]);
                const double margin = Eps * (clipMagnitude[axis] +
                    (withW ? clipMagnitude[3] : 0.0)) + underflowMargin;
                return maximum < -margin;
            };
            return reject(0,1,true) || reject(0,-1,true) ||
                reject(1,1,true) || reject(1,-1,true) ||
                reject(2,1,false) || reject(2,-1,true);
        }

    private:
        static bool Safe(double v) noexcept { return std::isfinite(v) && std::abs(v) <= Limit; }
        Rows m_Camera{}, m_AbsCamera{};
        bool m_Valid = true;
    };
}
