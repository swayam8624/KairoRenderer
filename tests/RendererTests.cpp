#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>

import Kairo.Renderer;
import Kairo.Foundation.Math;
import Kairo.Assets.MeshArtifact;

using namespace kairo::renderer;

TEST_CASE("Renderer window descriptions validate required dimensions", "[KairoRenderer][Types]")
{
    REQUIRE_NOTHROW(ValidateWindowDesc({ "Test", 1, 1, false }));
    REQUIRE_THROWS(ValidateWindowDesc({ "", 1, 1, false }));
    REQUIRE_THROWS(ValidateWindowDesc({ "Test", 0, 1, false }));
}

TEST_CASE("Vulkan backend snapshots reject incomplete integration handles", "[KairoRenderer][Backend]")
{
    const VulkanBackendContext context;
    CHECK_FALSE(context.IsValid());
    const VulkanOverlayRecorder recorder;
    CHECK_FALSE(static_cast<bool>(recorder));
}

TEST_CASE("Directional shadow settings reject unsafe runtime tuning", "[KairoRenderer][Shadow]")
{
    DirectionalShadowSettings settings;
    REQUIRE_NOTHROW(settings.Validate());
    settings.Strength = 1.01f;
    REQUIRE_THROWS(settings.Validate());
    settings = {};
    settings.ReceiverBias = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS(settings.Validate());
    settings = {};
    settings.ConstantDepthBias = -0.01f;
    REQUIRE_THROWS(settings.Validate());
    settings = {};
    settings.SlopeDepthBias = 16.01f;
    REQUIRE_THROWS(settings.Validate());
}

TEST_CASE("Showcase camera produces Vulkan-depth projection and advances its model", "[KairoRenderer][Camera]")
{
    ShowcaseCamera camera;
    const auto initialModel = camera.Model();
    const auto projection = camera.Projection(1600u, 900u);

    camera.Advance(0.5f);

    CHECK(camera.Model() != initialModel);
    CHECK(projection(0, 0) > 0.0f);
    CHECK(projection(1, 1) < 0.0f);
    CHECK(projection(3, 2) == -1.0f);
}

TEST_CASE("Renderer camera poses reject degenerate views and drive the view matrix", "[KairoRenderer][Camera]")
{
    CameraPose pose;
    REQUIRE_NOTHROW(pose.Validate());
    ShowcaseCamera camera;
    const auto initial = camera.View();
    pose.Position = { 6.0f, 3.0f, 2.0f };
    pose.Target = { 1.0f, 0.0f, -1.0f };
    camera.SetPose(pose);
    CHECK(camera.View() != initial);

    pose.Target = pose.Position;
    REQUIRE_THROWS_AS(pose.Validate(), std::invalid_argument);
}

TEST_CASE("Debug draw emits deterministic AABB edges and axes", "[KairoRenderer][Debug]")
{
    DebugDrawList draw;
    draw.AddAABB({ -1.0f, -2.0f, -3.0f }, { 1.0f, 2.0f, 3.0f });
    REQUIRE(draw.Lines().size() == 12u);
    draw.AddAxes({ 0.0f, 0.0f, 0.0f }, 2.0f);
    CHECK(draw.Lines().size() == 15u);
    REQUIRE_THROWS(draw.AddAABB({ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 1.0f }));
}

TEST_CASE("Indexed mesh validates topology and exposes a complete cube", "[KairoRenderer][Mesh]")
{
    const Mesh cube = Mesh::MakeCube();
    CHECK(cube.Vertices().size() == 24u);
    CHECK(cube.Indices().size() == 36u);
    REQUIRE_THROWS(Mesh({ {{ 0.0f, 0.0f, 0.0f }, {} } }, { 0u, 1u, 0u }));
}

TEST_CASE("Renderer primitive mesh factories expose valid blockout geometry", "[KairoRenderer][Mesh]")
{
    const Mesh plane = Mesh::MakePlane();
    const Mesh sphere = Mesh::MakeUVSphere(8u, 12u);
    const Mesh cylinder = Mesh::MakeCylinder(12u);
    CHECK(plane.Indices().size() == 6u);
    CHECK_FALSE(sphere.Vertices().empty());
    CHECK(sphere.Indices().size() == 8u * 12u * 6u);
    CHECK_FALSE(cylinder.Vertices().empty());
    CHECK(cylinder.Indices().size() == 12u * 12u);
    REQUIRE_THROWS_AS(Mesh::MakeUVSphere(2u, 12u), std::invalid_argument);
    REQUIRE_THROWS_AS(Mesh::MakeCylinder(2u), std::invalid_argument);
}

TEST_CASE("Renderer mesh consumes the shared portable asset contract", "[KairoRenderer][Mesh][Assets]")
{
    kairo::assets::MeshArtifactData artifact;
    artifact.HasNormals = true;
    artifact.Vertices = {
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} },
        { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} },
        { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} }
    };
    artifact.Indices = { 0u, 1u, 2u };

    const Mesh mesh = Mesh::FromArtifact(artifact, { 0.25f, 0.5f, 0.75f });
    REQUIRE(mesh.Vertices().size() == 3u);
    CHECK(mesh.Indices() == artifact.Indices);
    CHECK(mesh.Vertices()[1u].Position == kairo::foundation::math::Vec3f{ 1.0f, 0.0f, 0.0f });
    CHECK(mesh.Vertices()[1u].Normal == kairo::foundation::math::Vec3f{ 0.0f, 0.0f, 1.0f });
    CHECK(mesh.Vertices()[1u].Color == kairo::foundation::math::Vec3f{ 0.25f, 0.5f, 0.75f });

    artifact.HasNormals = false;
    for (auto& vertex : artifact.Vertices) vertex.Normal = {};
    REQUIRE_THROWS_AS(Mesh::FromArtifact(artifact), std::invalid_argument);
    artifact.HasNormals = true;
    for (auto& vertex : artifact.Vertices) vertex.Normal = { 0.0f, 0.0f, 1.0f };
    REQUIRE_THROWS_AS(Mesh::FromArtifact(
        artifact, { std::numeric_limits<float>::infinity(), 1.0f, 1.0f }), std::invalid_argument);
}

TEST_CASE("Render scenes validate draw handles transforms and tints", "[KairoRenderer][Scene]")
{
    RenderScene scene;
    scene.Add({ 1u });
    REQUIRE(scene.Draws().size() == 1u);
    REQUIRE_THROWS(scene.Add({ InvalidMeshHandle }));

    MeshDraw invalidMatrix{ 1u };
    invalidMatrix.Model(2u, 1u) = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS(scene.Add(invalidMatrix));
    REQUIRE_THROWS(scene.Add({ 1u, kairo::foundation::math::MakeScale(kairo::foundation::math::Vec3f{ 1.0f, 0.0f, 1.0f }) }));
    REQUIRE_THROWS(scene.Add({ 1u, kairo::foundation::math::Mat4f::Identity(), { { 1.0f, -0.1f, 1.0f } } }));

    PBRMaterial invalidMaterial;
    invalidMaterial.Metallic = 1.1f;
    REQUIRE_THROWS(scene.Add({ 1u, kairo::foundation::math::Mat4f::Identity(), invalidMaterial }));
    invalidMaterial = {};
    invalidMaterial.Roughness = 0.0f;
    REQUIRE_THROWS(scene.Add({ 1u, kairo::foundation::math::Mat4f::Identity(), invalidMaterial }));

    const auto normal = ComputeNormalMatrix(
        kairo::foundation::math::MakeScale(kairo::foundation::math::Vec3f{ 2.0f, 1.0f, 0.5f }));
    CHECK(normal(0u, 0u) == 1.0f);
    CHECK(normal(1u, 1u) == 2.0f);
    CHECK(normal(2u, 2u) == 4.0f);
}

TEST_CASE("Debug draw emits sphere, capsule, and contact geometry", "[KairoRenderer][Debug]")
{
    DebugDrawList draw;
    draw.AddWireSphere({ 0.0f, 0.0f, 0.0f }, 1.0f, 8u);
    REQUIRE(draw.Lines().size() == 24u);
    draw.AddWireCapsule({ 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.5f, 4u);
    CHECK(draw.Lines().size() == 52u);
    draw.AddContactNormal({ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
    CHECK(draw.Lines().size() == 53u);
}

TEST_CASE("Debug draw preserves oriented box geometry", "[KairoRenderer][Debug]")
{
    DebugDrawList draw;
    const auto rotation = kairo::foundation::math::AxisAngle(
        kairo::foundation::math::Vec3f::Up(), 1.57079632679f);
    draw.AddOBB({ 2.0f, 3.0f, 4.0f }, { 1.0f, 0.5f, 2.0f }, rotation);

    REQUIRE(draw.Lines().size() == 12u);
    CHECK(draw.Lines().front().A.y == 2.5f);
    CHECK(draw.Lines().front().B.y == 2.5f);
    REQUIRE_THROWS(draw.AddOBB({}, { 1.0f, 0.0f, 1.0f }, rotation));
    REQUIRE_THROWS(draw.AddOBB({}, { 1.0f, 1.0f, 1.0f }, kairo::foundation::math::Quatf::Zero()));
}
