#include <catch2/catch_test_macros.hpp>

import Kairo.Foundation.Geometry;
import Kairo.Foundation.Math;
import Kairo.Foundation.PhysicsEngine;
import Kairo.Renderer.PhysicsDebugBridge;

using namespace kairo::renderer;
using namespace kairo::foundation::physics;

TEST_CASE("Physics debug bridge translates every collider vocabulary", "[KairoRenderer][PhysicsDebug]")
{
    using kairo::foundation::math::Vec3f;
    std::vector<DebugShape> shapes;
    DebugShape sphere; sphere.Kind = DebugShapeKind::Sphere; sphere.Radius = 0.5f;
    DebugShape capsule; capsule.Kind = DebugShapeKind::Capsule; capsule.Radius = 0.25f;
    capsule.SegmentStart = { 0.0f, -1.0f, 0.0f }; capsule.SegmentEnd = { 0.0f, 1.0f, 0.0f }; capsule.Sleeping = true;
    DebugShape aabb; aabb.Kind = DebugShapeKind::AABB; aabb.HalfExtents = { 1.0f, 2.0f, 3.0f };
    DebugShape box; box.Kind = DebugShapeKind::Box; box.HalfExtents = { 1.0f, 1.0f, 1.0f };
    box.Rotation = kairo::foundation::math::AxisAngle(Vec3f::Up(), 0.5f); box.IsTrigger = true;
    DebugShape plane; plane.Kind = DebugShapeKind::Plane; plane.PlaneNormal = Vec3f::Up();
    shapes = { sphere, capsule, aabb, box, plane };

    PhysicsDebugDrawOptions options;
    options.CurvedShapeSegments = 4u;
    options.PlaneHalfExtent = 1.0f;
    options.PlaneGridSpacing = 1.0f;
    const auto draw = BuildPhysicsDebugDraw(shapes, {}, {}, options);

    // sphere 12 + capsule 28 + AABB 12 + OBB 12 + three plane rows/columns 6
    CHECK(draw.Lines().size() == 70u);
    CHECK(draw.Lines()[0].Color.G == 0.95f);
    CHECK(draw.Lines()[12].Color.R == 0.48f);
    CHECK(draw.Lines()[52].Color.B == 1.0f);
}

TEST_CASE("Physics debug bridge toggles bounds and contact markers", "[KairoRenderer][PhysicsDebug]")
{
    using kairo::foundation::geometry::AABBf;
    PhysicsDebugDrawOptions options;
    options.DrawColliderShapes = false;
    options.DrawBroadphaseAABBs = true;
    const std::vector<DebugAABB> bounds{ { 4u, AABBf::FromMinMax({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }) } };
    const std::vector<DebugContact> contacts{ { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.1f } };

    const auto draw = BuildPhysicsDebugDraw({}, bounds, contacts, options);
    CHECK(draw.Lines().size() == 16u);

    options.CurvedShapeSegments = 2u;
    REQUIRE_THROWS_AS(BuildPhysicsDebugDraw({}, {}, {}, options), std::invalid_argument);
}

TEST_CASE("Physics world snapshots feed the renderer bridge", "[KairoRenderer][PhysicsDebug]")
{
    PhysicsWorld world;
    RigidBodyDesc body;
    body.State.Position = { 1.0f, 2.0f, 3.0f };
    const auto id = world.CreateRigidBody(body);
    [[maybe_unused]] const auto collider = world.AddCollider(id, SphereCollider{ 0.5f });

    PhysicsDebugDrawOptions options;
    options.CurvedShapeSegments = 4u;
    options.DrawContacts = false;
    const auto draw = BuildPhysicsDebugDraw(world, options);

    REQUIRE(draw.Lines().size() == 12u);
    CHECK(draw.Lines().front().A.x == 1.5f);
    CHECK(draw.Lines().front().A.y == 2.0f);
}
