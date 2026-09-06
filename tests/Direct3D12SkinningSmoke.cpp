#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

import Kairo.Renderer;
import Kairo.Foundation.Math;
import Kairo.Assets;

int main()
{
    try
    {
        using namespace kairo::renderer;
        using kairo::foundation::math::MakeTranslation;
        using kairo::foundation::math::Vec3f;

        kairo::assets::GltfPrimitiveData primitive;
        primitive.Mesh.HasNormals = true;
        primitive.Mesh.Vertices = {
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} },
            { {  0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} },
            { {  0.0f,  0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} }
        };
        primitive.Mesh.Indices = { 0u, 1u, 2u };
        primitive.Skinning.resize(3u);
        for (auto& influence : primitive.Skinning)
        {
            influence.Joints = { 0u, 999999u, 70000u, 255u };
            influence.Weights = { 1.0f, 0.0f, 0.0f, 0.0f };
        }

        WindowDesc window{ "D3D12 skinning smoke", 96u, 96u, false };
        window.Backend = GraphicsBackend::Direct3D12;
        RendererRuntime runtime(window);
        if (runtime.Backend() != GraphicsBackend::Direct3D12)
            throw std::runtime_error(
                "D3D12 skinning smoke did not instantiate the requested backend");
        const MeshHandle mesh = runtime.CreateMesh(Mesh::FromGltfPrimitive(primitive));
        if (mesh == InvalidMeshHandle)
            throw std::runtime_error("D3D12 skinned mesh upload returned an invalid handle");

        // A skinned mesh cannot be submitted without enough joint matrices.
        // Exercise this native runtime boundary before the valid render path.
        bool rejectedMissingPalette = false;
        try
        {
            MeshDraw invalidDraw;
            invalidDraw.Mesh = mesh;
            RenderScene invalidScene;
            invalidScene.Add(invalidDraw);
            runtime.SubmitRenderScene(invalidScene);
        }
        catch (const std::invalid_argument&)
        {
            rejectedMissingPalette = true;
        }
        if (!rejectedMissingPalette)
            throw std::runtime_error(
                "D3D12 accepted a skinned draw without its required joint palette");

        RenderLight light;
        light.Type = RenderLightType::Directional;
        light.Direction = { 0.25f, 1.0f, 0.35f };
        light.CastShadows = true;
        runtime.SetViewportShadingMode(ViewportShadingMode::Unlit);

        MeshDraw draw;
        draw.Mesh = mesh;
        draw.Skinning.JointMatrices.push_back(
            MakeTranslation(Vec3f{ -0.2f, 0.0f, 0.0f }));
        RenderScene firstScene;
        firstScene.Add(draw);
        firstScene.AddLight(light);
        runtime.SubmitRenderScene(firstScene);
        runtime.RequestViewportCapture();
        runtime.DrawFrame();
        auto first = runtime.TakeViewportCapture();
        if (!first.has_value() || !first->IsVisuallyNonUniform())
            throw std::runtime_error(
                "first D3D12 skinned frame did not produce visible geometry");
        if (first->Width != 96u || first->Height != 96u)
            throw std::runtime_error("first D3D12 capture extent is incorrect");

        draw.Skinning.JointMatrices[0] =
            MakeTranslation(Vec3f{ 0.35f, 0.0f, 0.0f });
        RenderScene secondScene;
        secondScene.Add(draw);
        secondScene.AddLight(light);
        runtime.SubmitRenderScene(secondScene);
        runtime.RequestViewportCapture();
        runtime.DrawFrame();
        auto second = runtime.TakeViewportCapture();
        if (!second.has_value() || !second->IsVisuallyNonUniform())
            throw std::runtime_error(
                "second D3D12 skinned frame did not produce visible geometry");
        if (second->Width != 96u || second->Height != 96u)
            throw std::runtime_error("second D3D12 capture extent is incorrect");
        if (first->RGBA == second->RGBA)
            throw std::runtime_error(
                "changing the D3D12 joint palette did not change rendered pixels");
        return 0;
    }
    catch (const kairo::renderer::PresentationUnavailableError& error)
    {
        std::cerr << "D3D12 presentation unavailable: " << error.what() << '\n';
        return 77;
    }
    catch (const std::exception& error)
    {
        std::cerr << "D3D12 skinning smoke failed: " << error.what() << '\n';
        return 1;
    }
}
