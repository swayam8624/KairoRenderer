#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

struct GLFWwindow;

namespace kairo::renderer::detail
{
    /// Plain ABI vertex copied from KairoRenderer's module-owned MeshVertex.
    /// Keeping this header free of module and Objective-C declarations lets the
    /// C++23 runtime own validation while Objective-C++ owns Metal lifetimes.
    struct MetalVertex final
    {
        float Position[3];
        float Color[3];
        float Normal[3];
        float TexCoord[2];
    };

    struct MetalSkinInfluence final
    {
        std::uint32_t Joints[4]{};
        float Weights[4]{};
    };

    struct MetalDebugVertex final
    {
        float Position[3];
        float Color[4];
    };

    enum class MetalTextureFormat : std::uint8_t
    {
        R8Linear,
        RG8Linear,
        RGBA8Linear,
        RGBA8SRGB,
        RGBA16Float
    };

    enum class MetalAddressMode : std::uint8_t { Repeat, Clamp, Mirror };
    enum class MetalFilter : std::uint8_t { Nearest, Linear };

    struct MetalTextureUpload final
    {
        std::uint32_t Width = 0;
        std::uint32_t Height = 0;
        std::uint32_t MipLevels = 0;
        MetalTextureFormat Format = MetalTextureFormat::RGBA8Linear;
        MetalAddressMode AddressU = MetalAddressMode::Repeat;
        MetalAddressMode AddressV = MetalAddressMode::Repeat;
        MetalFilter MinFilter = MetalFilter::Linear;
        MetalFilter MagFilter = MetalFilter::Linear;
        const std::byte* Bytes = nullptr;
        std::size_t ByteCount = 0;
    };

    struct MetalMaterial final
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

    struct MetalDraw final
    {
        std::uint64_t Mesh = 0u;
        float Model[16]{};
        float Normal[9]{};
        MetalMaterial Material{};
        std::uint32_t CastShadows = 1u;
        // Borrowed only for the synchronous Draw() encoding call. Matrices are
        // already column-major and are copied into Metal command data before
        // Draw returns.
        const float* SkinMatrices = nullptr;
        std::uint32_t SkinJointCount = 0u;
    };

    struct alignas(16) MetalLight final
    {
        float PositionType[4]{};
        float DirectionRange[4]{};
        float ColorIntensity[4]{};
        float SpotArea[4]{};
        std::uint32_t CastShadows = 1u;
        std::uint32_t Padding[3]{};
    };

    struct MetalFrame final
    {
        std::span<const MetalDraw> Draws;
        std::span<const MetalLight> Lights;
        std::span<const MetalDebugVertex> DebugVertices;
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

    struct MetalCapture final
    {
        std::uint32_t Width = 0u;
        std::uint32_t Height = 0u;
        std::unique_ptr<std::uint8_t[]> RGBA;
        std::size_t ByteCount = 0u;
    };

    /// Native tooling callback. Both arguments are borrowed Objective-C object
    /// pointers (`id<MTLCommandBuffer>`, `id<MTLRenderCommandEncoder>`), erased
    /// here so no public C++ module must compile as Objective-C++.
    using MetalOverlayRecorder =
        std::function<void(void* commandBuffer, void* renderEncoder)>;

    class MetalBackend final
    {
    public:
        MetalBackend(GLFWwindow* window, std::uint32_t width,
            std::uint32_t height);
        ~MetalBackend();
        MetalBackend(const MetalBackend&) = delete;
        MetalBackend& operator=(const MetalBackend&) = delete;

        [[nodiscard]] std::uint64_t CreateMesh(std::span<const MetalVertex> vertices,
            std::span<const std::uint32_t> indices,
            std::span<const MetalSkinInfluence> skinning = {});
        void DestroyMesh(std::uint64_t handle);
        [[nodiscard]] std::uint64_t CreateTexture(const MetalTextureUpload& upload);
        void DestroyTexture(std::uint64_t handle);
        void Resize(std::uint32_t width, std::uint32_t height);
        void SetDrawableSize(std::uint32_t width, std::uint32_t height);
        void Draw(const MetalFrame& frame);
        [[nodiscard]] std::uint32_t Pick(std::uint32_t x, std::uint32_t y);
        [[nodiscard]] MetalCapture Capture();
        [[nodiscard]] void* ViewportTexture() const noexcept;
        [[nodiscard]] void* Device() const noexcept;
        [[nodiscard]] std::uint32_t Width() const noexcept;
        [[nodiscard]] std::uint32_t Height() const noexcept;
        void SetOverlayRecorder(MetalOverlayRecorder recorder);

    private:
        class Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
