#include <exception>
#include <iostream>

import Kairo.Renderer;

int main()
{
    try
    {
        kairo::renderer::RendererRuntime runtime({ "KairoRenderer M6 - Debug Draw", 1280, 800, true });
        kairo::renderer::DebugDrawList debug;
        debug.AddAxes({ 0.0f, 0.0f, 0.0f }, 2.5f);
        debug.AddAABB({ -2.0f, -1.5f, -2.0f }, { 2.0f, 1.5f, 2.0f }, { 0.25f, 0.8f, 1.0f, 1.0f });
        debug.AddWireSphere({ 0.0f, 0.25f, 0.0f }, 1.25f, 24u, { 1.0f, 0.75f, 0.2f, 1.0f });
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
