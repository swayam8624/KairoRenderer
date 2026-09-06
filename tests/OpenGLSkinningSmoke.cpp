#include <exception>
#include <iostream>

import Kairo.Renderer;
import Kairo.Foundation.Math;
import Kairo.Assets;

int main()
{
    try
    {
        using namespace kairo::renderer;
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
            influence.Joints = { 0u, 0u, 0u, 0u };
            influence.Weights = { 1.0f, 0.0f, 0.0f, 0.0f };
        }

        OpenGLRendererRuntime runtime({ "OpenGL skinning smoke", 96u, 96u, false });
        const MeshHandle mesh = runtime.CreateMesh(Mesh::FromGltfPrimitive(primitive));
        RenderScene scene;
        MeshDraw draw;
        draw.Mesh = mesh;
        draw.Skinning.JointMatrices.push_back(
            kairo::foundation::math::MakeTranslation(
                kairo::foundation::math::Vec3f{ 0.1f, 0.0f, 0.0f }));
        scene.Add(draw);
        RenderLight light;
        light.Type = RenderLightType::Directional;
        light.Direction = { 0.25f, 1.0f, 0.35f };
        light.CastShadows = true;
        scene.AddLight(light);
        runtime.SetViewportShadingMode(ViewportShadingMode::Unlit);
        runtime.SubmitRenderScene(scene);
        runtime.DrawFrame();
        return 0;
    }
    catch (const PresentationUnavailableError& error)
    {
        std::cerr << "OpenGL presentation unavailable: " << error.what() << '\n';
        return 77;
    }
    catch (const std::exception& error)
    {
        std::cerr << "OpenGL skinning smoke failed: " << error.what() << '\n';
        return 1;
    }
}
