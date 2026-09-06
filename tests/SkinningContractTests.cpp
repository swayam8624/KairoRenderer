#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

import Kairo.Assets;
import Kairo.Foundation.Math;
import Kairo.Renderer;

using namespace kairo::assets;
using namespace kairo::renderer;

namespace
{
    [[nodiscard]] GltfPrimitiveData SkinnedPrimitive()
    {
        GltfPrimitiveData primitive;
        primitive.Mesh.Vertices = {
            { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, {} },
            { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, {} },
            { { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, {} }
        };
        primitive.Mesh.Indices = { 0u, 1u, 2u };
        primitive.Mesh.HasNormals = true;
        primitive.Skinning.resize(3u);
        primitive.Skinning[0] = { { 0u, 1u, 0u, 0u }, { 0.75f, 0.25f, 0.0f, 0.0f } };
        primitive.Skinning[1] = { { 1u, 0u, 0u, 0u }, { 1.0f, 0.0f, 0.0f, 0.0f } };
        primitive.Skinning[2] = { { 0u, 0u, 0u, 0u }, { 1.0f, 0.0f, 0.0f, 0.0f } };
        return primitive;
    }
}

TEST_CASE("renderer skin influences require normalized finite weights")
{
    SkinVertexInfluence valid{ { 0u, 1u, 0u, 0u }, { 0.75f, 0.25f, 0.0f, 0.0f } };
    REQUIRE_NOTHROW(ValidateSkinVertexInfluence(valid));

    auto invalid = valid;
    invalid.Weights[0] = -0.1f;
    REQUIRE_THROWS_AS(ValidateSkinVertexInfluence(invalid), std::invalid_argument);

    invalid = valid;
    invalid.Weights[0] = 0.5f;
    REQUIRE_THROWS_AS(ValidateSkinVertexInfluence(invalid), std::invalid_argument);

    invalid = valid;
    invalid.Weights[0] = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS_AS(ValidateSkinVertexInfluence(invalid), std::invalid_argument);
}

TEST_CASE("renderer mesh preserves glTF skin stream without changing static vertex layout")
{
    const auto primitive = SkinnedPrimitive();
    const Mesh mesh = Mesh::FromGltfPrimitive(primitive);
    REQUIRE(mesh.IsSkinned());
    REQUIRE(mesh.Skinning().size() == mesh.Vertices().size());
    CHECK(mesh.Skinning()[0].Joints[1] == 1u);
    CHECK(mesh.Skinning()[0].Weights[0] == 0.75f);
    CHECK(mesh.RequiredJointCount() == 2u);

    const Mesh staticMesh = Mesh::FromArtifact(primitive.Mesh);
    CHECK_FALSE(staticMesh.IsSkinned());
    CHECK(staticMesh.Skinning().empty());
    CHECK(staticMesh.RequiredJointCount() == 0u);
}

TEST_CASE("skin palette validation rejects nonfinite joint transforms")
{
    SkinPalette palette;
    palette.JointMatrices.push_back(kairo::foundation::math::Mat4f::Identity());
    REQUIRE_NOTHROW(palette.Validate());
    palette.JointMatrices[0](2u, 1u) = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(palette.Validate(), std::invalid_argument);
}

TEST_CASE("glTF render adapter marks skinned primitives and keeps palette space asset relative")
{
    GltfSceneArtifactData scene;
    scene.Primitives.push_back(SkinnedPrimitive());

    GltfSkinData skin;
    skin.Joints = { 0u, 1u };
    skin.InverseBindMatrices = {
        { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 },
        { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }
    };
    scene.Skins.push_back(skin);

    GltfNodeData root;
    root.HasRestTRS = true;
    scene.Nodes.push_back(root);

    GltfNodeData meshNode;
    meshNode.Parent = 0;
    meshNode.HasRestTRS = true;
    meshNode.RestTranslation = { 3.0f, 0.0f, 0.0f };
    meshNode.PrimitiveIndices = { 0u };
    meshNode.SkinIndex = 0u;
    scene.Nodes.push_back(meshNode);
    scene.RootNodes = { 0u };
    ValidateGltfSceneArtifactData(scene);

    const auto render = MakeGltfRenderAsset(scene);
    REQUIRE(render.Primitives.size() == 1u);
    const auto& primitive = render.Primitives.front();
    CHECK(primitive.Geometry.IsSkinned());
    CHECK(primitive.SkinIndex == 0u);
    // Skinned palette output is already in imported asset space. Therefore the
    // per-primitive adapter transform must not apply the mesh-node world again.
    CHECK(primitive.LocalToAsset == kairo::foundation::math::Mat4f::Identity());
}

TEST_CASE("render scene carries a finite backend-neutral skin palette")
{
    MeshDraw draw;
    draw.Mesh = 1u;
    draw.Skinning.JointMatrices.push_back(kairo::foundation::math::Mat4f::Identity());
    REQUIRE_NOTHROW(RenderScene::Validate(draw));

    draw.Skinning.JointMatrices[0](0u, 0u) =
        std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(RenderScene::Validate(draw), std::invalid_argument);
}
