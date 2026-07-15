module;

#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.DebugDraw;

import Kairo.Foundation.Math;

export namespace kairo::renderer
{
    /// Renderer-owned debug primitive. Physics and gameplay code translate
    /// into this plain data rather than importing Vulkan or renderer modules.
    struct DebugColor final
    {
        float R = 1.0f;
        float G = 1.0f;
        float B = 1.0f;
        float A = 1.0f;
    };

    struct DebugLine final
    {
        kairo::foundation::math::Vec3f A{};
        kairo::foundation::math::Vec3f B{};
        DebugColor Color{};
    };

    /// Input: world-space line primitives.
    /// Output: an append-only, frame-local line list.
    /// Task: form the stable physics/debug boundary. Call Clear after the
    /// renderer consumes a frame; this type deliberately contains no GPU state.
    class DebugDrawList final
    {
    public:
        void Clear() noexcept { m_Lines.clear(); }
        [[nodiscard]] bool Empty() const noexcept { return m_Lines.empty(); }
        [[nodiscard]] const std::vector<DebugLine>& Lines() const noexcept { return m_Lines; }

        void AddLine(const kairo::foundation::math::Vec3f& a, const kairo::foundation::math::Vec3f& b, DebugColor color = {})
        {
            m_Lines.push_back({ a, b, color });
        }

        /// Input: finite min/max corners in world space.
        /// Output: twelve box edges.
        /// Task: visualize broadphase and collider bounds without requiring a
        /// geometry package in the public debug API.
        void AddAABB(const kairo::foundation::math::Vec3f& minimum, const kairo::foundation::math::Vec3f& maximum, DebugColor color = {})
        {
            if (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z)
            {
                throw std::invalid_argument("DebugDrawList::AddAABB requires ordered minimum/maximum corners.");
            }
            const std::array corners{
                kairo::foundation::math::Vec3f{ minimum.x, minimum.y, minimum.z },
                kairo::foundation::math::Vec3f{ maximum.x, minimum.y, minimum.z },
                kairo::foundation::math::Vec3f{ maximum.x, maximum.y, minimum.z },
                kairo::foundation::math::Vec3f{ minimum.x, maximum.y, minimum.z },
                kairo::foundation::math::Vec3f{ minimum.x, minimum.y, maximum.z },
                kairo::foundation::math::Vec3f{ maximum.x, minimum.y, maximum.z },
                kairo::foundation::math::Vec3f{ maximum.x, maximum.y, maximum.z },
                kairo::foundation::math::Vec3f{ minimum.x, maximum.y, maximum.z }
            };
            constexpr std::array<std::array<std::size_t, 2>, 12> edges{{
                {{ 0u, 1u }}, {{ 1u, 2u }}, {{ 2u, 3u }}, {{ 3u, 0u }},
                {{ 4u, 5u }}, {{ 5u, 6u }}, {{ 6u, 7u }}, {{ 7u, 4u }},
                {{ 0u, 4u }}, {{ 1u, 5u }}, {{ 2u, 6u }}, {{ 3u, 7u }}
            }};
            for (const auto& edge : edges) AddLine(corners[edge[0]], corners[edge[1]], color);
        }

        void AddAxes(const kairo::foundation::math::Vec3f& origin, float scale = 1.0f)
        {
            if (scale <= 0.0f) throw std::invalid_argument("DebugDrawList::AddAxes requires a positive scale.");
            AddLine(origin, origin + kairo::foundation::math::Vec3f{ scale, 0.0f, 0.0f }, { 0.95f, 0.2f, 0.2f, 1.0f });
            AddLine(origin, origin + kairo::foundation::math::Vec3f{ 0.0f, scale, 0.0f }, { 0.2f, 0.95f, 0.35f, 1.0f });
            AddLine(origin, origin + kairo::foundation::math::Vec3f{ 0.0f, 0.0f, scale }, { 0.3f, 0.5f, 1.0f, 1.0f });
        }

        /// Task: emit three orthogonal wire circles. `segments` must be at
        /// least three so every ring is a valid closed polygon.
        void AddWireSphere(const kairo::foundation::math::Vec3f& center, float radius, std::size_t segments = 24u, DebugColor color = {})
        {
            if (radius <= 0.0f || segments < 3u) throw std::invalid_argument("Wire sphere requires positive radius and at least three segments.");
            constexpr float Tau = 6.28318530718f;
            for (std::size_t index = 0u; index < segments; ++index)
            {
                const float a = Tau * static_cast<float>(index) / static_cast<float>(segments);
                const float b = Tau * static_cast<float>((index + 1u) % segments) / static_cast<float>(segments);
                AddLine(center + kairo::foundation::math::Vec3f{ std::cos(a) * radius, std::sin(a) * radius, 0.0f }, center + kairo::foundation::math::Vec3f{ std::cos(b) * radius, std::sin(b) * radius, 0.0f }, color);
                AddLine(center + kairo::foundation::math::Vec3f{ std::cos(a) * radius, 0.0f, std::sin(a) * radius }, center + kairo::foundation::math::Vec3f{ std::cos(b) * radius, 0.0f, std::sin(b) * radius }, color);
                AddLine(center + kairo::foundation::math::Vec3f{ 0.0f, std::cos(a) * radius, std::sin(a) * radius }, center + kairo::foundation::math::Vec3f{ 0.0f, std::cos(b) * radius, std::sin(b) * radius }, color);
            }
        }

        /// Task: display the capsule center segment plus endpoint spheres.
        /// The physics convention is world-space endpoints and a scalar radius.
        void AddWireCapsule(const kairo::foundation::math::Vec3f& a, const kairo::foundation::math::Vec3f& b, float radius, std::size_t segments = 16u, DebugColor color = {})
        {
            if (radius <= 0.0f) throw std::invalid_argument("Wire capsule requires a positive radius.");
            AddWireSphere(a, radius, segments, color);
            AddWireSphere(b, radius, segments, color);
            AddLine(a + kairo::foundation::math::Vec3f{ radius, 0.0f, 0.0f }, b + kairo::foundation::math::Vec3f{ radius, 0.0f, 0.0f }, color);
            AddLine(a - kairo::foundation::math::Vec3f{ radius, 0.0f, 0.0f }, b - kairo::foundation::math::Vec3f{ radius, 0.0f, 0.0f }, color);
            AddLine(a + kairo::foundation::math::Vec3f{ 0.0f, 0.0f, radius }, b + kairo::foundation::math::Vec3f{ 0.0f, 0.0f, radius }, color);
            AddLine(a - kairo::foundation::math::Vec3f{ 0.0f, 0.0f, radius }, b - kairo::foundation::math::Vec3f{ 0.0f, 0.0f, radius }, color);
        }

        void AddContactNormal(const kairo::foundation::math::Vec3f& point, const kairo::foundation::math::Vec3f& normal, float scale = 0.25f, DebugColor color = { 1.0f, 0.85f, 0.15f, 1.0f })
        {
            if (scale <= 0.0f) throw std::invalid_argument("Contact-normal scale must be positive.");
            AddLine(point, point + normal * scale, color);
        }

    private:
        std::vector<DebugLine> m_Lines;
    };
}
