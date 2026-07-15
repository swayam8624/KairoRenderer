module;

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

export module Kairo.Renderer.PhysicsDebugBridge;

import Kairo.Foundation.Math;
import Kairo.Foundation.PhysicsEngine;
import Kairo.Renderer.DebugDraw;

export namespace kairo::renderer
{
    /// Controls renderer-side tessellation of immutable PhysicsEngine debug
    /// records. These settings affect visualization only and never mutate or
    /// step the simulated world.
    struct PhysicsDebugDrawOptions final
    {
        bool DrawColliderShapes = true;
        bool DrawBroadphaseAABBs = false;
        bool DrawContacts = true;
        std::size_t CurvedShapeSegments = 20u;
        float ContactNormalScale = 0.3f;
        float ContactMarkerRadius = 0.045f;
        float PlaneHalfExtent = 10.0f;
        float PlaneGridSpacing = 1.0f;
    };

    /// Input: renderer-side debug tessellation settings.
    /// Output: throws before allocation for unsafe or degenerate values.
    inline void ValidatePhysicsDebugDrawOptions(const PhysicsDebugDrawOptions& options)
    {
        if (options.CurvedShapeSegments < 3u || options.CurvedShapeSegments > 512u)
            throw std::invalid_argument("Physics debug curved-shape segments must be in [3, 512].");
        if (!(options.ContactNormalScale > 0.0f) || !std::isfinite(options.ContactNormalScale) ||
            !(options.ContactMarkerRadius > 0.0f) || !std::isfinite(options.ContactMarkerRadius))
            throw std::invalid_argument("Physics debug contact scales must be finite and positive.");
        if (!(options.PlaneHalfExtent > 0.0f) || !std::isfinite(options.PlaneHalfExtent) ||
            !(options.PlaneGridSpacing > 0.0f) || !std::isfinite(options.PlaneGridSpacing) ||
            options.PlaneHalfExtent / options.PlaneGridSpacing > 2048.0f)
            throw std::invalid_argument("Physics debug plane grid settings are invalid or excessively dense.");
    }

    namespace physics_debug_detail
    {
        using kairo::foundation::math::Vec3f;

        [[nodiscard]] inline DebugColor ShapeColor(const kairo::foundation::physics::DebugShape& shape) noexcept
        {
            if (shape.IsTrigger) return { 0.2f, 0.85f, 1.0f, 1.0f };
            if (shape.Sleeping) return { 0.48f, 0.53f, 0.6f, 1.0f };
            return { 0.3f, 0.95f, 0.45f, 1.0f };
        }

        inline void AddPlaneGrid(DebugDrawList& result,
            const kairo::foundation::physics::DebugShape& shape,
            const PhysicsDebugDrawOptions& options,
            DebugColor color)
        {
            const Vec3f normal = kairo::foundation::math::SafeNormalize(shape.PlaneNormal, Vec3f::Up());
            const Vec3f reference = std::abs(normal.y) < 0.9f ? Vec3f::Up() : Vec3f::Right();
            const Vec3f tangent = kairo::foundation::math::SafeNormalize(
                kairo::foundation::math::Cross(reference, normal), Vec3f::Right());
            const Vec3f bitangent = kairo::foundation::math::Cross(normal, tangent);
            const Vec3f origin = normal * -shape.PlaneDistance;
            const std::size_t halfLines = static_cast<std::size_t>(std::floor(options.PlaneHalfExtent / options.PlaneGridSpacing));
            for (std::size_t index = 0u; index <= halfLines * 2u; ++index)
            {
                const float offset = (static_cast<float>(index) - static_cast<float>(halfLines)) * options.PlaneGridSpacing;
                result.AddLine(origin + tangent * -options.PlaneHalfExtent + bitangent * offset,
                    origin + tangent * options.PlaneHalfExtent + bitangent * offset, color);
                result.AddLine(origin + bitangent * -options.PlaneHalfExtent + tangent * offset,
                    origin + bitangent * options.PlaneHalfExtent + tangent * offset, color);
            }
        }
    }

    /// Input: copied PhysicsEngine shape, AABB, and contact records.
    /// Output: renderer-owned line primitives in the same right-handed world
    /// coordinate system.
    /// Task: form the one-way PhysicsEngine -> Renderer integration boundary.
    /// PhysicsEngine remains independent of this module and of all GPU APIs.
    [[nodiscard]] inline DebugDrawList BuildPhysicsDebugDraw(
        const std::vector<kairo::foundation::physics::DebugShape>& shapes,
        const std::vector<kairo::foundation::physics::DebugAABB>& broadphaseBounds,
        const std::vector<kairo::foundation::physics::DebugContact>& contacts,
        const PhysicsDebugDrawOptions& options = {})
    {
        using namespace kairo::foundation::physics;
        using kairo::foundation::math::Vec3f;
        ValidatePhysicsDebugDrawOptions(options);
        DebugDrawList result;

        if (options.DrawColliderShapes)
        {
            for (const DebugShape& shape : shapes)
            {
                const DebugColor color = physics_debug_detail::ShapeColor(shape);
                switch (shape.Kind)
                {
                case DebugShapeKind::Sphere:
                    result.AddWireSphere(shape.Center, shape.Radius, options.CurvedShapeSegments, color);
                    break;
                case DebugShapeKind::Capsule:
                    result.AddWireCapsule(shape.SegmentStart, shape.SegmentEnd, shape.Radius, options.CurvedShapeSegments, color);
                    break;
                case DebugShapeKind::Plane:
                    physics_debug_detail::AddPlaneGrid(result, shape, options, color);
                    break;
                case DebugShapeKind::AABB:
                    result.AddAABB(shape.Center - shape.HalfExtents, shape.Center + shape.HalfExtents, color);
                    break;
                case DebugShapeKind::Box:
                    result.AddOBB(shape.Center, shape.HalfExtents, shape.Rotation, color);
                    break;
                }
            }
        }

        if (options.DrawBroadphaseAABBs)
            for (const DebugAABB& bounds : broadphaseBounds)
                result.AddAABB(bounds.Bounds.Min, bounds.Bounds.Max, { 0.25f, 0.55f, 1.0f, 1.0f });

        if (options.DrawContacts)
        {
            for (const DebugContact& contact : contacts)
            {
                const float radius = options.ContactMarkerRadius;
                constexpr DebugColor contactColor{ 1.0f, 0.78f, 0.12f, 1.0f };
                result.AddLine(contact.Position - Vec3f::Right() * radius, contact.Position + Vec3f::Right() * radius, contactColor);
                result.AddLine(contact.Position - Vec3f::Up() * radius, contact.Position + Vec3f::Up() * radius, contactColor);
                result.AddLine(contact.Position - Vec3f::Backward() * radius, contact.Position + Vec3f::Backward() * radius, contactColor);
                result.AddContactNormal(contact.Position, contact.Normal, options.ContactNormalScale, contactColor);
            }
        }
        return result;
    }

    /// Convenience overload that snapshots a PhysicsWorld through its public
    /// debug API. The returned list owns all data and remains valid after the
    /// world advances again.
    [[nodiscard]] inline DebugDrawList BuildPhysicsDebugDraw(
        const kairo::foundation::physics::PhysicsWorld& world,
        const PhysicsDebugDrawOptions& options = {})
    {
        return BuildPhysicsDebugDraw(world.DebugShapes(), world.DebugAABBs(), world.DebugContacts(), options);
    }
}
