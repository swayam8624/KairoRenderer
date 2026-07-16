module;

#include <cmath>
#include <cstdint>
#include <stdexcept>

export module Kairo.Renderer.Camera;

import Kairo.Foundation.Math;

export namespace kairo::renderer
{
    /// Renderer-neutral camera pose used by games, tools, and the editor.
    ///
    /// Input: a right-handed world-space eye position and focus target.
    /// Output: a validated camera request with no windowing or GPU state.
    /// Task: let a host own camera interaction while the renderer owns only
    /// matrix construction and GPU upload. The direction from `Position` to
    /// `Target` must be non-zero; the world up vector is +Y.
    struct CameraPose final
    {
        kairo::foundation::math::Vec3f Position{ 2.6f, 1.9f, 3.8f };
        kairo::foundation::math::Vec3f Target{};
        kairo::foundation::math::Vec3f Up = kairo::foundation::math::Vec3f::Up();

        void Validate() const
        {
            using namespace kairo::foundation::math;
            const auto finite = [](const Vec3f& value)
            {
                return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
            };
            if (!finite(Position) || !finite(Target) || !finite(Up) ||
                (Target - Position).LengthSquared() <= 1.0e-10f ||
                Up.LengthSquared() <= 1.0e-10f)
                throw std::invalid_argument("CameraPose requires finite position/target and a non-zero direction/up vector.");
        }
    };

    /// Input: framebuffer extent and elapsed seconds.
    /// Output: stable right-handed view, Vulkan-depth projection, and animated
    /// model matrices.
    /// Task: provide a small reusable camera contract without coupling render
    /// code to windowing input. Matrices remain KairoMath row-major on the CPU;
    /// GPU upload handles the required storage conversion separately.
    class ShowcaseCamera final
    {
    public:
        void Advance(float elapsedSeconds) noexcept
        {
            m_AngleRadians = std::fmod(m_AngleRadians + elapsedSeconds * 0.72f, 6.28318530718f);
        }

        [[nodiscard]] kairo::foundation::math::Mat4f Model() const noexcept
        {
            return kairo::foundation::math::MakeRotationY(m_AngleRadians) *
                kairo::foundation::math::MakeRotationX(m_AngleRadians * 0.35f);
        }

        [[nodiscard]] kairo::foundation::math::Mat4f View() const noexcept
        {
            using namespace kairo::foundation::math;
            return LookAt(m_Pose.Position, m_Pose.Target, m_Pose.Up);
        }

        [[nodiscard]] static constexpr kairo::foundation::math::Vec3f Position() noexcept
        {
            return { 2.6f, 1.9f, 3.8f };
        }

        /// Input: a validated editor/game camera pose.
        /// Output: subsequent frames use its view matrix.
        /// Task: replace the former fixed showcase-only camera path without
        /// coupling renderer input to GLFW, ImGui, or editor types.
        void SetPose(const CameraPose& pose)
        {
            pose.Validate();
            m_Pose = pose;
        }

        [[nodiscard]] const CameraPose& Pose() const noexcept { return m_Pose; }

        [[nodiscard]] kairo::foundation::math::Mat4f Projection(std::uint32_t width, std::uint32_t height) const noexcept
        {
            using namespace kairo::foundation::math;
            Mat4f projection = Perspective(
                1.0471975512f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 100.0f);
            // The renderer uses Vulkan's conventional positive-height
            // viewport. Flip clip-space Y once in the projection so +Y world
            // geometry appears upward in the framebuffer.
            projection(1u, 1u) *= -1.0f;
            return projection;
        }

    private:
        float m_AngleRadians = 0.0f;
        CameraPose m_Pose{};
    };
}
