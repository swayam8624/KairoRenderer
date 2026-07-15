#include <exception>
#include <iostream>
#include <stdexcept>

import Kairo.Renderer;
import Kairo.Foundation.Math;

int main()
{
    try
    {
        kairo::renderer::RendererRuntime runtime({ "KairoRenderer M10 - PBR Directional Shadows", 1280, 800, true });
        if (!runtime.BackendContext().IsValid())
        {
            throw std::runtime_error("Renderer did not expose a valid Vulkan tooling context.");
        }
        const auto cube = runtime.CreateMesh(kairo::renderer::Mesh::MakeCube());
        kairo::renderer::RenderScene scene;
        using kairo::foundation::math::Vec3f;
        scene.Add({ cube, kairo::foundation::math::MakeTranslation(Vec3f{ -0.8f, -0.47f, 0.0f }) *
            kairo::foundation::math::MakeScale(Vec3f{ 0.6f, 0.6f, 0.6f }), { { 0.95f, 0.45f, 0.35f }, 0.05f, 0.72f, 1.0f } });
        scene.Add({ cube, kairo::foundation::math::MakeTranslation(Vec3f{ 0.8f, -0.17f, 0.0f }) *
            kairo::foundation::math::MakeScale(Vec3f{ 0.5f, 0.9f, 0.5f }), { { 0.82f, 0.9f, 1.0f }, 0.85f, 0.22f, 1.0f } });
        // A broad, shallow cube provides a closed receiving surface with valid
        // normals and makes both penumbra filtering and depth bias visible.
        scene.Add({ cube, kairo::foundation::math::MakeTranslation(Vec3f{ 0.0f, -1.15f, 0.0f }) *
            kairo::foundation::math::MakeScale(Vec3f{ 2.5f, 0.08f, 2.0f }), { { 0.52f, 0.58f, 0.62f }, 0.0f, 0.88f, 1.0f } });
        runtime.SubmitRenderScene(scene);
        kairo::renderer::DebugDrawList debug;
        debug.AddAxes({ 0.0f, -1.05f, 0.0f }, 1.4f);
        runtime.SubmitDebugDraw(debug);
        while (!runtime.NativeWindow().ShouldClose())
        {
            runtime.NativeWindow().PollEvents();
            runtime.DrawFrame();
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "KairoRenderer error: " << error.what() << '\n';
        return 1;
    }
}
