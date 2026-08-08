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
import Kairo.Renderer.Texture;

export namespace kairo::renderer
{
    /// Stable runtime key for renderer-owned mesh buffers. Zero is reserved so
    /// value-initialized draw commands fail validation instead of drawing an
    /// arbitrary resource.
    using MeshHandle = std::uint64_t;
    inline constexpr MeshHandle InvalidMeshHandle = 0u;

    enum class RenderLightType : std::uint8_t
    {
        Directional = 1u,
        Point = 2u,
        Spot = 3u,
        RectangleArea = 4u
    };

    inline constexpr std::size_t MaximumRenderLights = 16u;

    /// Input: world-space position/direction and photometric intensity already
    /// converted by the EngineCore adapter. Direction points from the shaded
    /// surface toward a directional light. Spot cone values are half-angles.
    struct RenderLight final
    {
        RenderLightType Type = RenderLightType::Directional;
        kairo::foundation::math::Vec3f Position{};
        kairo::foundation::math::Vec3f Direction{ 0.0f, 1.0f, 0.0f };
        kairo::foundation::math::Vec3f Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        float InnerConeRadians = 0.35f;
        float OuterConeRadians = 0.65f;
        float AreaWidth = 1.0f;
        float AreaHeight = 1.0f;
        bool CastShadows = true;

        void Validate() const;
    };

    /// Global renderer state selected from EngineCore's deterministic active
    /// environment. Environment maps remain a texture handle so Vulkan owns
    /// sampling resources while scene files retain persistent asset IDs.
    struct RenderEnvironment final
    {
        kairo::foundation::math::Vec3f BackgroundColor{ 0.035f, 0.055f, 0.075f };
        kairo::foundation::math::Vec3f AmbientColor{ 1.0f, 1.0f, 1.0f };
        float AmbientIntensity = 0.08f;
        float ExposureEV100 = 0.0f;
        TextureHandle EnvironmentTexture = InvalidTextureHandle;

        void Validate() const;
    };

    /// Input: a valid mesh handle, object-to-world matrix, and PBR factors.
    /// Output: one indexed draw request for the next submitted scene.
    /// Task: keep frame extraction independent from Vulkan command structures.
    struct MeshDraw final
    {
        MeshHandle Mesh = InvalidMeshHandle;
        kairo::foundation::math::Mat4f Model = kairo::foundation::math::Mat4f::Identity();
        PBRMaterial Material{};
        /// Zero means non-pickable. Editor scene extraction supplies stable
        /// scene entity IDs; runtime-only draws may deliberately leave it zero.
        std::uint32_t ObjectID = 0u;
        bool CastShadows = true;
        bool ReceiveShadows = true;
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
        [[nodiscard]] const std::vector<RenderLight>& Lights() const noexcept { return m_Lights; }
        [[nodiscard]] const RenderEnvironment& Environment() const noexcept { return m_Environment; }

        void Add(MeshDraw draw)
        {
            Validate(draw);
            m_Draws.push_back(draw);
        }

        void AddLight(RenderLight light)
        {
            if (m_Lights.size() >= MaximumRenderLights)
                throw std::length_error("RenderScene exceeds the 16-light forward-pass limit.");
            light.Validate();
            m_Lights.push_back(light);
        }

        void SetEnvironment(RenderEnvironment environment)
        {
            environment.Validate();
            m_Environment = environment;
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
        std::vector<RenderLight> m_Lights;
        RenderEnvironment m_Environment;
    };

    inline void RenderLight::Validate() const
    {
        const auto finite3 = [](const auto& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (!finite3(Position) || !finite3(Direction) || !finite3(Color) ||
            Color.x < 0.0f || Color.y < 0.0f || Color.z < 0.0f ||
            !std::isfinite(Intensity) || Intensity < 0.0f ||
            !std::isfinite(Range) || Range <= 0.0f ||
            !std::isfinite(AreaWidth) || AreaWidth <= 0.0f ||
            !std::isfinite(AreaHeight) || AreaHeight <= 0.0f)
            throw std::invalid_argument("Render light vectors, color, intensity, and range must be finite and physical.");
        const float directionLengthSquared = Direction.x * Direction.x +
            Direction.y * Direction.y + Direction.z * Direction.z;
        if (directionLengthSquared <= std::numeric_limits<float>::epsilon())
            throw std::invalid_argument("Render light direction cannot be zero.");
        if (!std::isfinite(InnerConeRadians) || !std::isfinite(OuterConeRadians) ||
            InnerConeRadians < 0.0f || OuterConeRadians <= InnerConeRadians ||
            OuterConeRadians >= 1.57079632679f)
            throw std::invalid_argument("Render spot cones must satisfy 0 <= inner < outer < pi/2.");
        switch (Type)
        {
            case RenderLightType::Directional:
            case RenderLightType::Point:
            case RenderLightType::Spot:
            case RenderLightType::RectangleArea: break;
            default: throw std::invalid_argument("Render light type is invalid.");
        }
    }

    inline void RenderEnvironment::Validate() const
    {
        const auto validColor = [](const auto& color)
        {
            return std::isfinite(color.x) && std::isfinite(color.y) &&
                std::isfinite(color.z) && color.x >= 0.0f &&
                color.y >= 0.0f && color.z >= 0.0f;
        };
        if (!validColor(BackgroundColor) || !validColor(AmbientColor) ||
            !std::isfinite(AmbientIntensity) || AmbientIntensity < 0.0f ||
            !std::isfinite(ExposureEV100) || ExposureEV100 < -32.0f ||
            ExposureEV100 > 32.0f)
            throw std::invalid_argument("Render environment values must be finite and within supported ranges.");
    }
}
