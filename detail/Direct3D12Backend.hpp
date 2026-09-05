#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

struct GLFWwindow;

namespace kairo::renderer::detail
{
    struct Direct3D12Vertex final
    {
        float Position[3];
        float Color[3];
        float Normal[3];
        float TexCoord[2];
    };

    struct Direct3D12DebugVertex final
    {
        float Position[3];
        float Color[4];
    };

    enum class Direct3D12TextureFormat : std::uint8_t
    {
        R8Linear,
        RG8Linear,
        RGBA8Linear,
        RGBA8SRGB,
        RGBA16Float
    };

    enum class Direct3D12AddressMode : std::uint8_t { Repeat, Clamp, Mirror };
    enum class Direct3D12Filter : std::uint8_t { Nearest, Linear };

    struct Direct3D12TextureUpload final
    {
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::uint32_t MipLevels = 0u;
        Direct3D12TextureFormat Format = Direct3D12TextureFormat::RGBA8Linear;
        Direct3D12AddressMode AddressU = Direct3D12AddressMode::Repeat;
        Direct3D12AddressMode AddressV = Direct3D12AddressMode::Repeat;
        Direct3D12Filter MinFilter = Direct3D12Filter::Linear;
        Direct3D12Filter MagFilter = Direct3D12Filter::Linear;
        const std::byte* Bytes = nullptr;
        std::size_t ByteCount = 0u;
    };

    struct Direct3D12Material final
    {
        float BaseColor[4]{};
        float Emissive[3]{};
        float Metallic = 0.0f;
        float Roughness = 1.0f;
        float AmbientOcclusion = 1.0f;
        float NormalScale = 1.0f;
        float AlphaCutoff = 0.5f;
        std::uint32_t AlphaMode = 1u;
        std::uint32_t ObjectID = 0u;
        bool DoubleSided = false;
        bool ReceiveShadows = true;
        std::uint64_t BaseColorTexture = 0u;
        std::uint64_t NormalTexture = 0u;
        std::uint64_t MetallicRoughnessTexture = 0u;
        std::uint64_t EmissiveTexture = 0u;
        std::uint64_t OcclusionTexture = 0u;
    };

    struct Direct3D12Draw final
    {
        std::uint64_t Mesh = 0u;
        float Model[16]{};
        float Normal[9]{};
        Direct3D12Material Material{};
        std::uint32_t CastShadows = 1u;
    };

    struct alignas(16) Direct3D12Light final
    {
        float PositionType[4]{};
        float DirectionRange[4]{};
        float ColorIntensity[4]{};
        float SpotArea[4]{};
        std::uint32_t CastShadows = 1u;
        std::uint32_t Padding[3]{};
    };

    struct Direct3D12Frame final
    {
        std::span<const Direct3D12Draw> Draws;
        std::span<const Direct3D12Light> Lights;
        std::span<const Direct3D12DebugVertex> DebugVertices;
        float View[16]{};
        float Projection[16]{};
        float LightViewProjection[16]{};
        float CameraPosition[3]{};
        float Background[3]{};
        float Ambient[3]{};
        float Exposure = 0.0f;
        float EnvironmentIntensity = 1.0f;
        std::uint64_t EnvironmentTexture = 0u;
        std::uint32_t ShadingMode = 0u;
        bool ShadowsEnabled = true;
        float ShadowStrength = 0.85f;
        float ReceiverBias = 0.0015f;
        float ConstantDepthBias = 1.25f;
        float SlopeDepthBias = 1.75f;
    };

    struct Direct3D12Descriptor final
    {
        std::uintptr_t CPU = 0u;
        std::uint64_t GPU = 0u;
    };

    struct Direct3D12Capture final
    {
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::unique_ptr<std::uint8_t[]> RGBA;
        std::size_t ByteCount = 0u;
    };

    using Direct3D12OverlayRecorder = std::function<void(void* commandList)>;

    class Direct3D12Backend final
    {
    public:
        Direct3D12Backend(GLFWwindow* window, std::uint32_t width,
            std::uint32_t height);
        ~Direct3D12Backend();
        Direct3D12Backend(const Direct3D12Backend&) = delete;
        Direct3D12Backend& operator=(const Direct3D12Backend&) = delete;

        [[nodiscard]] std::uint64_t CreateMesh(
            std::span<const Direct3D12Vertex> vertices,
            std::span<const std::uint32_t> indices);
        void DestroyMesh(std::uint64_t handle);
        [[nodiscard]] std::uint64_t CreateTexture(
            const Direct3D12TextureUpload& upload);
        void DestroyTexture(std::uint64_t handle);
        void Resize(std::uint32_t width, std::uint32_t height);
        void SetDrawableSize(std::uint32_t width, std::uint32_t height);
        void Draw(const Direct3D12Frame& frame);
        [[nodiscard]] std::uint32_t Pick(std::uint32_t x, std::uint32_t y);
        [[nodiscard]] Direct3D12Capture Capture();
        [[nodiscard]] Direct3D12Descriptor ViewportDescriptor() const noexcept;
        [[nodiscard]] void* Device() const noexcept;
        [[nodiscard]] void* CommandQueue() const noexcept;
        [[nodiscard]] void* ShaderResourceHeap() const noexcept;
        [[nodiscard]] Direct3D12Descriptor AllocateToolingDescriptor();
        void FreeToolingDescriptor(Direct3D12Descriptor descriptor);

        // BackendContext() is intentionally a const runtime accessor so Editor
        // integrations can query stable native handles without gaining general
        // mutation access to the renderer. Its descriptor callbacks are the one
        // controlled exception: allocating/freeing tooling descriptors mutates
        // only allocator bookkeeping, not frame/scene state. Keep that logical
        // mutability narrowly encapsulated here rather than making the runtime
        // or the entire backend member mutable.
        [[nodiscard]] Direct3D12Descriptor AllocateToolingDescriptor() const
        {
            return const_cast<Direct3D12Backend*>(this)->AllocateToolingDescriptor();
        }
        void FreeToolingDescriptor(Direct3D12Descriptor descriptor) const
        {
            const_cast<Direct3D12Backend*>(this)->FreeToolingDescriptor(descriptor);
        }

        [[nodiscard]] std::uint32_t Width() const noexcept;
        [[nodiscard]] std::uint32_t Height() const noexcept;
        void SetOverlayRecorder(Direct3D12OverlayRecorder recorder);

    private:
        class Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
