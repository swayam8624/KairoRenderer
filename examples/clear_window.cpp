#include <exception>
#include <iostream>

import Kairo.Renderer;

int main()
{
    try
    {
        kairo::renderer::RendererRuntime runtime({ "KairoRenderer M3 - Vulkan Clear", 1280, 800, true });
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
