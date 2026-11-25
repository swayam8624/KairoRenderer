#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace kairo::renderer::detail
{
    // Metal setVertexBytes is intended for at most 4 KiB. A portable
    // 255-joint palette needs 16,320 bytes. Pack all palettes into a
    // retained command-buffer resource instead, with aligned offsets.
    inline constexpr std::size_t MetalSkinAlignment = 256u;

    struct MetalSkinRange final
    {
        std::size_t Offset = 0u;
        std::size_t Bytes = 0u;
    };

    [[nodiscard]] inline MetalSkinRange AppendMetalSkinRange(
        std::size_t& total, std::size_t joints)
    {
        if (joints == 0u) return {};
        if (joints > 255u) throw std::length_error("Metal skin palette exceeds 255 joints.");
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        constexpr auto padding = MetalSkinAlignment - 1u;
        if (total > maximum - padding) throw std::length_error("Metal skin alignment overflow.");
        const auto offset = (total + padding) & ~padding;
        const auto bytes = joints * 16u * sizeof(float);
        if (offset > maximum - bytes) throw std::length_error("Metal skin upload overflow.");
        total = offset + bytes;
        return {offset, bytes};
    }
}
