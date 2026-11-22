#pragma once

// CPU-only, backend-independent packing plan for VulkanTexture2D::Upload.
// This is the exact byte/offset builder used by the real native uploader.
// It deliberately does not include vulkan.h, KairoAssets modules, or a GPU
// driver, so its overflow and failure behavior can be regression-tested
// under ordinary C++23 compilers.
//
// Input range element contract:
//     mip.Width, mip.Height           positive integral dimensions
//     mip.Pixels                      contiguous std::vector<std::byte> bytes
// The KairoAssets ValidateTextureArtifactData function remains the canonical
// validator for pixel format, expected byte count, semantic and mip chain.
// This helper adds a separate upload-offset and device-size trust boundary.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace kairo::renderer::detail
{
    struct TextureMipUploadRegion final
    {
        std::uint64_t ByteOffset = 0u;
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
    };

    struct TextureMipUploadPlan final
    {
        std::vector<std::byte> PackedBytes;
        std::vector<TextureMipUploadRegion> Regions;
    };

    // Vulkan's VkDeviceSize is 64 bits. The caller's host vector is sized
    // with size_t. Check BOTH limits before an addition or a reservation:
    // truncation of an offset could otherwise target the wrong mip in GPU
    // memory. The actual texture format/dimensions are checked upstream.
    template<class MipRange>
    [[nodiscard]] TextureMipUploadPlan MakeTextureMipUploadPlan(
        const MipRange& mips,
        std::uint64_t maximumUploadBytes = std::numeric_limits<std::uint64_t>::max())
    {
        if (mips.empty())
            throw std::invalid_argument("Texture upload requires at least one mip.");
        if (mips.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("Texture upload has too many mip levels.");

        TextureMipUploadPlan result;
        result.Regions.reserve(mips.size());

        std::uint64_t total = 0u;
        for (const auto& mip : mips)
        {
            if (mip.Width == 0u || mip.Height == 0u || mip.Pixels.empty())
                throw std::invalid_argument("Texture upload mip must have dimensions and bytes.");
            const std::size_t bytes = mip.Pixels.size();
            if (bytes > maximumUploadBytes ||
                total > maximumUploadBytes - static_cast<std::uint64_t>(bytes) ||
                total > std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(bytes))
                throw std::length_error("Texture mip upload exceeds its byte budget.");
            result.Regions.push_back({
                total, static_cast<std::uint32_t>(mip.Width),
                static_cast<std::uint32_t>(mip.Height)
            });
            total += static_cast<std::uint64_t>(bytes);
        }

        if (total > std::numeric_limits<std::size_t>::max() ||
            total > result.PackedBytes.max_size())
            throw std::length_error("Texture mip upload exceeds host vector address space.");
        result.PackedBytes.reserve(static_cast<std::size_t>(total));
        for (const auto& mip : mips)
            result.PackedBytes.insert(result.PackedBytes.end(),
                mip.Pixels.begin(), mip.Pixels.end());
        return result;
    }
}
