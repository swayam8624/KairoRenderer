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

        WindowDesc window{ "Metal skinning smoke", 96u, 96u, false };
        window.Backend = GraphicsBackend::Metal;
        RendererRuntime runtime(window);
        if (runtime.Backend() != GraphicsBackend::Metal)
            throw std::runtime_error(
                "Metal skinning smoke did not instantiate the requested backend");
        const MeshHandle mesh = runtime.CreateMesh(Mesh::FromGltfPrimitive(primitive));

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
                "first Metal skinned frame did not produce visible geometry");

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
                "second Metal skinned frame did not produce visible geometry");
        if (first->RGBA == second->RGBA)
            throw std::runtime_error(
                "changing the Metal joint palette did not change rendered pixels");
        return 0;
    }
    catch (const kairo::renderer::PresentationUnavailableError& error)
    {
        std::cerr << "Metal presentation unavailable: " << error.what() << '\n';
        return 77;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Metal skinning smoke failed: " << error.what() << '\n';
        return 1;
    }
}
