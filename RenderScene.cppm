module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.RenderScene;

import Kairo.Foundation.Math;
import Kairo.Renderer.Material;

export namespace kairo::renderer
{
    /// Stable runtime key for renderer-owned mesh buffers. Zero is reserved so
    /// value-initialized draw commands fail validation instead of drawing an
    /// arbitrary resource.
    using MeshHandle = std::uint64_t;
    inline constexpr MeshHandle InvalidMeshHandle = 0u;

    /// Input: a valid mesh handle, object-to-world matrix, and PBR factors.
    /// Output: one indexed draw request for the next submitted scene.
    /// Task: keep frame extraction independent from Vulkan command structures.
    struct MeshDraw final
    {
        MeshHandle Mesh = InvalidMeshHandle;
        kairo::foundation::math::Mat4f Model = kairo::foundation::math::Mat4f::Identity();
        PBRMaterial Material{};
    };

    /// Input: finite object-to-world matrix with a non-singular linear part.
    /// Output: inverse-transpose normal transform, normalized by a uniform
    /// scale factor that does not affect the fragment shader's unit normal.
    /// Task: preserve normals under non-uniform scale without performing a
    /// matrix inverse for every vertex. Scale normalization avoids determinant
    /// overflow/underflow before KairoMath performs the inversion.
    [[nodiscard]] inline kairo::foundation::math::Mat3f ComputeNormalMatrix(
        const kairo::foundation::math::Mat4f& model)
    {
        for (std::size_t row = 0u; row < 4u; ++row)
            for (std::size_t column = 0u; column < 4u; ++column)
                if (!std::isfinite(model(row, column))) throw std::invalid_argument("MeshDraw model matrix must be finite.");

        float maximum = 0.0f;
        for (std::size_t row = 0u; row < 3u; ++row)
            for (std::size_t column = 0u; column < 3u; ++column)
                maximum = std::max(maximum, std::abs(model(row, column)));
        if (maximum == 0.0f) throw std::invalid_argument("MeshDraw model matrix must have an invertible linear transform.");

        kairo::foundation::math::Mat3f normalizedLinear;
        for (std::size_t row = 0u; row < 3u; ++row)
            for (std::size_t column = 0u; column < 3u; ++column)
                normalizedLinear(row, column) = model(row, column) / maximum;
        if (std::abs(kairo::foundation::math::Determinant(normalizedLinear)) <= std::numeric_limits<float>::epsilon() * 16.0f)
            throw std::invalid_argument("MeshDraw model matrix must have an invertible linear transform.");
        return kairo::foundation::math::Transpose(kairo::foundation::math::Inverse(normalizedLinear));
    }

    /// Frame-local collection of opaque mesh draws. The renderer copies this
    /// list on submission; callers may immediately clear or reuse the source.
    class RenderScene final
    {
    public:
        void Clear() noexcept { m_Draws.clear(); }
        [[nodiscard]] bool Empty() const noexcept { return m_Draws.empty(); }
        [[nodiscard]] const std::vector<MeshDraw>& Draws() const noexcept { return m_Draws; }

        void Add(MeshDraw draw)
        {
            Validate(draw);
            m_Draws.push_back(draw);
        }

        /// Input: one draw description.
        /// Output: throws before GPU submission for invalid handles, non-finite
        /// transforms, or non-finite/negative linear color channels.
        static void Validate(const MeshDraw& draw)
        {
            if (draw.Mesh == InvalidMeshHandle) throw std::invalid_argument("MeshDraw requires a valid mesh handle.");
            static_cast<void>(ComputeNormalMatrix(draw.Model));
            draw.Material.Validate();
        }

    private:
        std::vector<MeshDraw> m_Draws;
    };
}
