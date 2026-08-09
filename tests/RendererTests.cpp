#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <vector>

import Kairo.Renderer;
import Kairo.Foundation.Math;
import Kairo.Assets.MeshArtifact;
import Kairo.Assets.MaterialArtifact;
import Kairo.Assets.AssetID;
import Kairo.Assets.Metadata;
import Kairo.Assets.GltfSceneArtifact;
import Kairo.Assets.TextureArtifact;

using namespace kairo::renderer;

TEST_CASE("Render graph compiles hazards transitions and transient aliases",
    "[KairoRenderer][RenderGraph]")
{
    RenderGraph graph;
    const auto shadow = graph.AddResource({ "Shadow", RenderResourceKind::Texture,
        4096u, true });
    const auto intermediate = graph.AddResource({ "Intermediate",
        RenderResourceKind::Texture, 4096u, true });
    const auto post = graph.AddResource({ "Post", RenderResourceKind::Texture,
        4096u, true });
    const auto swapchain = graph.AddResource({ "Swapchain",
        RenderResourceKind::External, 0u, false, RenderResourceState::Present });
    std::vector<int> execution;
    graph.AddPass("Shadow", { { shadow, RenderAccessMode::Write,
        RenderResourceState::DepthAttachment } }, [&] { execution.push_back(1); });
    graph.AddPass("Lighting", {
        { shadow, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { intermediate, RenderAccessMode::Write,
            RenderResourceState::ColorAttachment } },
        [&] { execution.push_back(2); });
    graph.AddPass("Post", {
        { intermediate, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { post, RenderAccessMode::Write, RenderResourceState::ColorAttachment } },
        [&] { execution.push_back(3); });
    graph.AddPass("Present", {
        { post, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { swapchain, RenderAccessMode::Write, RenderResourceState::Present } },
        [&] { execution.push_back(4); });

    const auto compiled = graph.Compile();
    REQUIRE(compiled.PassCount() == 4u);
    CHECK(compiled.PassName(0u) == "Shadow");
    CHECK(compiled.PassName(3u) == "Present");
    REQUIRE(compiled.Transitions(1u).size() == 2u);
    CHECK(compiled.Transitions(1u)[0].Before ==
        RenderResourceState::DepthAttachment);
    const auto profile = compiled.Execute();
    CHECK(execution == std::vector<int>{ 1, 2, 3, 4 });
    REQUIRE(profile.Passes.size() == 4u);
    CHECK(profile.TotalMilliseconds >= 0.0);
    REQUIRE(compiled.Lifetimes().size() == 4u);
    CHECK(compiled.Lifetimes()[shadow.Value].AliasSlot ==
        compiled.Lifetimes()[post.Value].AliasSlot);
    CHECK(compiled.Lifetimes()[shadow.Value].AliasSlot !=
        compiled.Lifetimes()[intermediate.Value].AliasSlot);
}

TEST_CASE("Render graph rejects invalid reads and dependency cycles",
    "[KairoRenderer][RenderGraph]")
{
    RenderGraph uninitialized;
    const auto resource = uninitialized.AddResource({ "Transient",
        RenderResourceKind::Buffer, 64u, true });
    uninitialized.AddPass("Read", { { resource, RenderAccessMode::Read,
        RenderResourceState::ShaderRead } });
    REQUIRE_THROWS_AS(uninitialized.Compile(), std::logic_error);

    RenderGraph cyclic;
    const auto external = cyclic.AddResource({ "External",
        RenderResourceKind::External, 0u, false,
        RenderResourceState::ShaderRead });
    const auto first = cyclic.AddPass("First", { { external,
        RenderAccessMode::Read, RenderResourceState::ShaderRead } });
    const auto second = cyclic.AddPass("Second", { { external,
        RenderAccessMode::Read, RenderResourceState::ShaderRead } });
    cyclic.DependsOn(first, second);
    cyclic.DependsOn(second, first);
    REQUIRE_THROWS_AS(cyclic.Compile(), std::logic_error);
}

TEST_CASE("Graphics backend selection is deterministic across desktop platforms",
    "[KairoRenderer][Backend]")
{
    const GraphicsBackendAvailability all{ true, true, true, true };
    CHECK(SelectGraphicsBackend(GraphicsBackend::Automatic, all, HostPlatform::MacOS) ==
        GraphicsBackend::Metal);
    CHECK(SelectGraphicsBackend(GraphicsBackend::Automatic, all, HostPlatform::Windows) ==
        GraphicsBackend::Direct3D12);
    CHECK(SelectGraphicsBackend(GraphicsBackend::Automatic, all, HostPlatform::Linux) ==
        GraphicsBackend::Vulkan);

    const GraphicsBackendAvailability portable{ true, false, false, true };
    CHECK(SelectGraphicsBackend(GraphicsBackend::Automatic, portable, HostPlatform::MacOS) ==
        GraphicsBackend::Vulkan);
    CHECK(SelectGraphicsBackend(GraphicsBackend::Automatic,
        { false, false, false, true }, HostPlatform::Windows) == GraphicsBackend::OpenGL);
}

TEST_CASE("Graphics backend requests reject unavailable or incompatible APIs",
    "[KairoRenderer][Backend]")
{
    CHECK(ParseGraphicsBackend("auto") == GraphicsBackend::Automatic);
    CHECK(ParseGraphicsBackend("d3d12") == GraphicsBackend::Direct3D12);
    CHECK(ParseGraphicsBackend("opengl") == GraphicsBackend::OpenGL);
    REQUIRE_THROWS_AS(ParseGraphicsBackend("software"), std::invalid_argument);
    REQUIRE_THROWS_AS(SelectGraphicsBackend(GraphicsBackend::Metal,
        { true, true, true, true }, HostPlatform::Windows), std::invalid_argument);
    REQUIRE_THROWS_AS(SelectGraphicsBackend(GraphicsBackend::Direct3D12,
        { true, true, false, true }, HostPlatform::Windows), std::runtime_error);
    REQUIRE_THROWS_AS(SelectGraphicsBackend(GraphicsBackend::Automatic,
        {}, HostPlatform::Linux), std::runtime_error);
}

TEST_CASE("Compiled graphics backend availability matches the host build",
    "[KairoRenderer][Backend]")
{
    const GraphicsBackendAvailability available = CompiledGraphicsBackends();
    CHECK(available.Vulkan);
    CHECK(available.OpenGL);
#if defined(__APPLE__)
    CHECK(available.Metal);
#else
    CHECK_FALSE(available.Metal);
#endif
#if defined(_WIN32)
    CHECK(available.Direct3D12);
#else
    CHECK_FALSE(available.Direct3D12);
#endif
}

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

TEST_CASE("Viewport captures reject uniform and malformed pixel evidence", "[KairoRenderer][Viewport]")
{
    ViewportCapture capture{ 2u, 1u, { 10u, 10u, 10u, 255u, 40u, 10u, 10u, 255u } };
    CHECK(capture.IsVisuallyNonUniform());
    capture.RGBA = { 10u, 10u, 10u, 255u, 10u, 10u, 10u, 255u };
    CHECK_FALSE(capture.IsVisuallyNonUniform());
    capture.RGBA.pop_back();
    CHECK_FALSE(capture.IsVisuallyNonUniform());
}

TEST_CASE("Viewport shading modes expose stable diagnostic names", "[KairoRenderer][Viewport]")
{
    CHECK(Name(ViewportShadingMode::Lit) == "Lit");
    CHECK(Name(ViewportShadingMode::Unlit) == "Unlit");
    CHECK(Name(ViewportShadingMode::Normals) == "Normals");
    CHECK(Name(ViewportShadingMode::Lighting) == "Lighting");
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
    CHECK(camera.Position() == pose.Position);

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
    CHECK(mesh.Vertices()[1u].TexCoord == kairo::foundation::math::Vec2f{});

    artifact.HasNormals = false;
    for (auto& vertex : artifact.Vertices) vertex.Normal = {};
    REQUIRE_THROWS_AS(Mesh::FromArtifact(artifact), std::invalid_argument);
    artifact.HasNormals = true;
    for (auto& vertex : artifact.Vertices) vertex.Normal = { 0.0f, 0.0f, 1.0f };
    REQUIRE_THROWS_AS(Mesh::FromArtifact(
        artifact, { std::numeric_limits<float>::infinity(), 1.0f, 1.0f }), std::invalid_argument);
}

TEST_CASE("Renderer material validates complete metallic roughness channels",
    "[KairoRenderer][Material]")
{
    PBRMaterial material;
    material.BaseColor = { 0.8f, 0.3f, 0.1f };
    material.BaseColorAlpha = 0.75f;
    material.Emissive = { 2.0f, 0.1f, 0.0f };
    material.NormalScale = 0.8f;
    material.AlphaMode = MaterialAlphaMode::Mask;
    material.AlphaCutoff = 0.4f;
    material.BaseColorTexture = 7u;
    REQUIRE_NOTHROW(material.Validate());

    material.NormalScale = -0.1f;
    REQUIRE_THROWS_AS(material.Validate(), std::invalid_argument);
    material = {};
    material.AlphaMode = static_cast<MaterialAlphaMode>(0u);
    REQUIRE_THROWS_AS(material.Validate(), std::invalid_argument);
}

TEST_CASE("Asset material conversion preserves every PBR channel",
    "[KairoRenderer][Material][Assets]")
{
    const auto textureID = kairo::assets::AssetID::Parse(
        "00000000-0000-4000-8000-000000000221");
    kairo::assets::MaterialArtifactData source;
    source.BaseColorFactor = { 0.2f, 0.4f, 0.6f, 0.75f };
    source.MetallicFactor = 0.8f;
    source.RoughnessFactor = 0.3f;
    source.EmissiveFactor = { 1.0f, 0.5f, 0.25f };
    source.NormalScale = 0.65f;
    source.OcclusionStrength = 0.45f;
    source.AlphaMode = kairo::assets::MaterialAlphaMode::Mask;
    source.AlphaCutoff = 0.35f;
    source.DoubleSided = true;
    source.Textures.BaseColor = kairo::assets::TextureAssetHandle{ textureID };

    const auto converted = MakePBRMaterial(source,
        [&](kairo::assets::TextureAssetHandle texture)
        {
            CHECK(texture.ID == textureID);
            return TextureHandle{ 17u };
        });
    CHECK(converted.BaseColor.x == 0.2f);
    CHECK(converted.BaseColorAlpha == 0.75f);
    CHECK(converted.Metallic == 0.8f);
    CHECK(converted.Roughness == 0.3f);
    CHECK(converted.Emissive.z == 0.25f);
    CHECK(converted.NormalScale == 0.65f);
    CHECK(converted.AmbientOcclusion == 0.45f);
    CHECK(converted.AlphaMode == MaterialAlphaMode::Mask);
    CHECK(converted.AlphaCutoff == 0.35f);
    CHECK(converted.DoubleSided);
    CHECK(converted.BaseColorTexture == 17u);
    CHECK_THROWS_AS(MakePBRMaterial(source), std::invalid_argument);
}

TEST_CASE("glTF scene conversion preserves hierarchy materials and texture semantics",
    "[KairoRenderer][Assets][glTF]")
{
    kairo::assets::MeshArtifactData triangle;
    triangle.HasNormals = true;
    triangle.HasTexCoords = true;
    triangle.Vertices = {
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
        { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } }
    };
    triangle.Indices = { 0u, 1u, 2u };

    kairo::assets::GltfMaterialData material;
    material.BaseColorFactor = { 0.25f, 0.5f, 0.75f, 0.8f };
    material.MetallicFactor = 0.6f;
    material.RoughnessFactor = 0.2f;
    material.AlphaMode = kairo::assets::GltfAlphaMode::Mask;
    material.BaseColorTexture.Uri = "albedo.png";
    material.NormalTexture.Uri = "normal.png";

    kairo::assets::GltfSceneArtifactData source;
    source.Materials.push_back(material);
    source.Primitives.push_back({ triangle, {}, 0u });
    kairo::assets::GltfNodeData parent;
    parent.Name = "Parent";
    parent.LocalTransform[12u] = 2.0f;
    kairo::assets::GltfNodeData child;
    child.Name = "Child";
    child.Parent = 0;
    child.LocalTransform[13u] = 3.0f;
    child.PrimitiveIndices = { 0u };
    source.Nodes = { parent, child };
    source.RootNodes = { 0u };

    std::vector<kairo::assets::TextureSemantic> semantics;
    const auto converted = MakeGltfRenderAsset(source,
        [&](std::string_view uri, kairo::assets::TextureSemantic semantic)
        {
            semantics.push_back(semantic);
            return uri == "albedo.png" ? TextureHandle{ 11u } : TextureHandle{ 12u };
        });
    REQUIRE(converted.Primitives.size() == 1u);
    const auto& primitive = converted.Primitives.front();
    CHECK(primitive.NodeIndex == 1u);
    CHECK(primitive.PrimitiveIndex == 0u);
    CHECK(primitive.LocalToAsset(0u, 3u) == 2.0f);
    CHECK(primitive.LocalToAsset(1u, 3u) == 3.0f);
    CHECK(primitive.Material.BaseColor ==
        kairo::foundation::math::Vec3f{ 0.25f, 0.5f, 0.75f });
    CHECK(primitive.Material.BaseColorTexture == 11u);
    CHECK(primitive.Material.NormalTexture == 12u);
    CHECK(primitive.Material.AlphaMode == MaterialAlphaMode::Mask);
    REQUIRE(semantics.size() == 2u);
    CHECK(semantics[0u] == kairo::assets::TextureSemantic::Color);
    CHECK(semantics[1u] == kairo::assets::TextureSemantic::Normal);
}

TEST_CASE("Render scenes validate authored lights and environment bounds",
    "[KairoRenderer][Scene][Lighting]")
{
    RenderScene scene;
    RenderLight directional;
    directional.Type = RenderLightType::Directional;
    directional.Direction = { -0.3f, 0.8f, 0.2f };
    directional.Intensity = 25'000.0f;
    scene.AddLight(directional);

    RenderLight point;
    point.Type = RenderLightType::Point;
    point.Position = { 2.0f, 4.0f, 1.0f };
    point.Intensity = 900.0f;
    point.Range = 12.0f;
    scene.AddLight(point);
    RenderLight area;
    area.Type = RenderLightType::RectangleArea;
    area.AreaWidth = 2.0f;
    area.AreaHeight = 3.0f;
    scene.AddLight(area);
    REQUIRE(scene.Lights().size() == 3u);

    RenderEnvironment environment;
    environment.BackgroundColor = { 0.02f, 0.03f, 0.05f };
    environment.AmbientIntensity = 0.2f;
    environment.EnvironmentIntensity = 1.5f;
    environment.ExposureEV100 = 1.0f;
    scene.SetEnvironment(environment);
    CHECK(scene.Environment().ExposureEV100 == 1.0f);
    CHECK(scene.Environment().EnvironmentIntensity == 1.5f);

    point.Direction = {};
    REQUIRE_THROWS_AS(point.Validate(), std::invalid_argument);
    area.AreaWidth = 0.0f;
    REQUIRE_THROWS_AS(area.Validate(), std::invalid_argument);
    environment.ExposureEV100 = 33.0f;
    REQUIRE_THROWS_AS(scene.SetEnvironment(environment), std::invalid_argument);
    environment.ExposureEV100 = 0.0f;
    environment.EnvironmentIntensity = -1.0f;
    REQUIRE_THROWS_AS(scene.SetEnvironment(environment), std::invalid_argument);
}

TEST_CASE("Render scenes validate draw handles transforms and tints", "[KairoRenderer][Scene]")
{
    RenderScene scene;
    MeshDraw identifiedDraw{ 1u };
    identifiedDraw.ObjectID = 42u;
    scene.Add(identifiedDraw);
    REQUIRE(scene.Draws().size() == 1u);
    CHECK(scene.Draws().front().ObjectID == 42u);
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
