module;

#include <cstdint>

export module Kairo.Renderer.Texture;

export namespace kairo::renderer
{
    /// Stable process-local key for a renderer-owned sampled image. Persistent
    /// asset identity remains in KairoAssets; this handle only identifies GPU
    /// storage owned by one RendererRuntime instance.
    using TextureHandle = std::uint64_t;
    inline constexpr TextureHandle InvalidTextureHandle = 0u;

    /// Sampler addressing requested by authored materials. Linear filtering is the default; pixel-art atlases opt into nearest sampling.
    enum class TextureAddressMode : std::uint8_t
    {
        Repeat = 1u,
        MirroredRepeat = 2u,
        ClampToEdge = 3u
    };

    struct TextureSampler final
    {
        TextureAddressMode AddressU = TextureAddressMode::Repeat;
        TextureAddressMode AddressV = TextureAddressMode::Repeat;
        bool Nearest = false;

        friend bool operator==(const TextureSampler&, const TextureSampler&) = default;
    };
}
