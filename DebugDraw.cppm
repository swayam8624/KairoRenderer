module;

#include <array>
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

    private:
        std::vector<DebugLine> m_Lines;
    };
}
