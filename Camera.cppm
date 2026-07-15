module;

#include <cmath>
#include <cstdint>

export module Kairo.Renderer.Camera;

import Kairo.Foundation.Math;

export namespace kairo::renderer
{
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
            return LookAt(Vec3f{ 2.6f, 1.9f, 3.8f }, Vec3f::Zero(), Vec3f::Up());
        }

        [[nodiscard]] kairo::foundation::math::Mat4f Projection(std::uint32_t width, std::uint32_t height) const noexcept
        {
            using namespace kairo::foundation::math;
            return Perspective(1.0471975512f, static_cast<float>(width) / static_cast<float>(height), 0.1f, 100.0f);
        }

    private:
        float m_AngleRadians = 0.0f;
    };
}
