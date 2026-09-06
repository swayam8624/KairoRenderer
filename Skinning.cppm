module;

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.Skinning;

import Kairo.Foundation.Math;

export namespace kairo::renderer
{
    /// Conservative first-generation GPU skin palette ceiling. 255 mat4 values
    /// occupy 16,320 bytes, fitting inside the 16 KiB uniform-block guarantee
    /// shared by the OpenGL 4.1 / Vulkan portability floor while remaining
    /// straightforward to mirror into Metal and D3D12 constant buffers.
    inline constexpr std::size_t MaximumSkinJoints = 255u;

    /// Four-weight linear-blend skinning input associated with one mesh vertex.
    /// Joint indices address the draw's SkinPalette in the exact order authored
    /// by the imported skin. Static meshes carry no influence stream at all.
    struct SkinVertexInfluence final
    {
        std::array<std::uint32_t, 4u> Joints{};
        std::array<float, 4u> Weights{};

        friend bool operator==(const SkinVertexInfluence&, const SkinVertexInfluence&) = default;
    };

    inline void ValidateSkinVertexInfluence(const SkinVertexInfluence& influence)
    {
        float total = 0.0f;
        for (float weight : influence.Weights)
        {
            if (!std::isfinite(weight) || weight < 0.0f)
                throw std::invalid_argument(
                    "Renderer skin weights must be finite and non-negative.");
            total += weight;
        }
        if (std::abs(total - 1.0f) > 1.0e-3f)
            throw std::invalid_argument(
                "Renderer skin weights must sum to one for a skinned vertex.");
        for (std::size_t slot = 0u; slot < influence.Joints.size(); ++slot)
            if (influence.Weights[slot] > 0.0f &&
                influence.Joints[slot] >= MaximumSkinJoints)
                throw std::out_of_range(
                    "Renderer skin joint index exceeds the portable GPU palette limit.");
    }

    /// Per-draw asset-space joint transforms produced by EngineCore. Empty is
    /// the explicit static-mesh state. The renderer deliberately knows nothing
    /// about animation clips, skeleton hierarchy, or inverse-bind derivation.
    struct SkinPalette final
    {
        std::vector<kairo::foundation::math::Mat4f> JointMatrices;

        [[nodiscard]] bool Empty() const noexcept { return JointMatrices.empty(); }
        [[nodiscard]] std::size_t Size() const noexcept { return JointMatrices.size(); }

        void Validate() const
        {
            if (JointMatrices.size() > MaximumSkinJoints)
                throw std::length_error(
                    "Renderer skin palette exceeds the portable 255-joint limit.");
            for (const auto& matrix : JointMatrices)
                for (std::size_t row = 0u; row < 4u; ++row)
                    for (std::size_t column = 0u; column < 4u; ++column)
                        if (!std::isfinite(matrix(row, column)))
                            throw std::invalid_argument(
                                "Renderer skin palette matrices must be finite.");
        }

        friend bool operator==(const SkinPalette&, const SkinPalette&) = default;
    };
}