// GPU-free regression for the exact byte/offset plan consumed by
// VulkanTexture2D::Upload. This tests malformed mip and capacity boundaries
// without creating any VkDevice, VkBuffer, or VkImage.
// Production KairoAssets validation additionally checks format, semantics,
// canonical downsampled extents and exact expected pixel byte counts.
#include "../detail/TextureMipUploadPlan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
    struct Mip final
    {
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::vector<std::byte> Pixels;
    };

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template<class Action, class Exception>
    void RequireThrows(Action&& action, const char* message)
    {
        bool thrown = false;
        try { action(); }
        catch (const Exception&) { thrown = true; }
        Require(thrown, message);
    }
}

int main()
{
    try
    {
        using kairo::renderer::detail::MakeTextureMipUploadPlan;
        // Hand-calculated 4x4 RGBA8 + 2x2 RGBA8 + 1x1 RGBA8:
        // byte offsets 0,64,80 and total 84 bytes. The byte arrays are
        // intentionally distinct to prove the staging copy has no overlap.
        const std::vector<Mip> mips{
            { 4u, 4u, std::vector<std::byte>(64u, std::byte{0x11}) },
            { 2u, 2u, std::vector<std::byte>(16u, std::byte{0x22}) },
            { 1u, 1u, std::vector<std::byte>(4u, std::byte{0x33}) }
        };
        const auto plan = MakeTextureMipUploadPlan(mips);
        Require(plan.Regions.size() == 3u && plan.PackedBytes.size() == 84u,
            "Wrong number of mip regions or staging byte size");
        Require(plan.Regions[0].ByteOffset == 0u &&
            plan.Regions[1].ByteOffset == 64u &&
            plan.Regions[2].ByteOffset == 80u,
            "Mip offset calculation is not packed in source mip order");
        Require(plan.Regions[0].Width == 4u && plan.Regions[0].Height == 4u &&
            plan.Regions[1].Width == 2u && plan.Regions[1].Height == 2u &&
            plan.Regions[2].Width == 1u && plan.Regions[2].Height == 1u,
            "Mip extent was not transferred into the staging plan");
        for (std::size_t i = 0; i < plan.PackedBytes.size(); ++i)
        {
            const auto expected = i < 64u ? std::byte{0x11} :
                i < 80u ? std::byte{0x22} : std::byte{0x33};
            Require(plan.PackedBytes[i] == expected,
                "Mip source bytes were reordered or overwritten");
        }

        // Exactly the upload budget is legal. A single byte more must
        // fail before any large std::vector staging allocation occurs.
        Require(MakeTextureMipUploadPlan(mips, 84u).PackedBytes.size() == 84u,
            "Exact-boundary payload incorrectly rejected");
        RequireThrows<decltype([&] { (void)MakeTextureMipUploadPlan(mips, 83u); }),
            std::length_error>(
                [&] { (void)MakeTextureMipUploadPlan(mips, 83u); },
                "One byte above upload budget was accepted");

        RequireThrows<decltype([&] { (void)MakeTextureMipUploadPlan(std::vector<Mip>{}); }),
            std::invalid_argument>(
                [&] { (void)MakeTextureMipUploadPlan(std::vector<Mip>{}); },
                "Empty mip chain was accepted");
        const std::vector<Mip> missingBytes{
            { 1u, 1u, {} }
        };
        RequireThrows<decltype([&] { (void)MakeTextureMipUploadPlan(missingBytes); }),
            std::invalid_argument>(
                [&] { (void)MakeTextureMipUploadPlan(missingBytes); },
                "Mip with missing bytes was accepted");
        const std::vector<Mip> zeroWidth{
            { 0u, 1u, std::vector<std::byte>(1u, std::byte{1}) }
        };
        RequireThrows<decltype([&] { (void)MakeTextureMipUploadPlan(zeroWidth); }),
            std::invalid_argument>(
                [&] { (void)MakeTextureMipUploadPlan(zeroWidth); },
                "Zero-width source mip was accepted");

        // No need to allocate a huge image to exercise the budget gate.
        RequireThrows<decltype([&] { (void)MakeTextureMipUploadPlan(mips, 1u); }),
            std::length_error>(
                [&] { (void)MakeTextureMipUploadPlan(mips, 1u); },
                "A too-small device upload budget was accepted");

        std::puts("KairoRenderer texture mip upload staging plan tests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Texture mip staging regression: %s\n", error.what());
        return 1;
    }
}
