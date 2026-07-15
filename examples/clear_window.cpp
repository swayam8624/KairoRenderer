#include <exception>
#include <iostream>
#include <stdexcept>

import Kairo.Renderer;
import Kairo.Foundation.Math;

int main()
{
    try
    {
        kairo::renderer::RendererRuntime runtime({ "KairoRenderer M8 - Lit Mesh and Debug Draw", 1280, 800, true });
        if (!runtime.BackendContext().IsValid())
        {
            throw std::runtime_error("Renderer did not expose a valid Vulkan tooling context.");
        }
        const auto cube = runtime.CreateMesh(kairo::renderer::Mesh::MakeCube());
        kairo::renderer::RenderScene scene;
        using kairo::foundation::math::Vec3f;
        scene.Add({ cube, kairo::foundation::math::MakeTranslation(Vec3f{ -0.8f, 0.0f, 0.0f }) *
            kairo::foundation::math::MakeScale(Vec3f{ 0.6f, 0.6f, 0.6f }), { 1.0f, 0.75f, 0.75f } });
        scene.Add({ cube, kairo::foundation::math::MakeTranslation(Vec3f{ 0.8f, 0.0f, 0.0f }) *
            kairo::foundation::math::MakeScale(Vec3f{ 0.5f, 0.9f, 0.5f }), { 0.7f, 0.85f, 1.0f } });
        runtime.SubmitRenderScene(scene);
        kairo::renderer::DebugDrawList debug;
        debug.AddAxes({ 0.0f, 0.0f, 0.0f }, 2.5f);
        debug.AddAABB({ -1.6f, -1.0f, -1.1f }, { 1.6f, 1.0f, 1.1f }, { 0.25f, 0.8f, 1.0f, 1.0f });
        debug.AddWireSphere({ 0.0f, 0.0f, 0.0f }, 0.75f, 24u, { 1.0f, 0.75f, 0.2f, 1.0f });
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
