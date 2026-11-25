#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
import Kairo.Renderer;

int main(int argc,char** argv)
{
    try
    {
        using namespace kairo::renderer;
        if(argc!=2)throw std::invalid_argument("Expected completion marker path.");
        const std::filesystem::path done=argv[1];
        RendererRuntime runtime({"Kairo Vulkan Presentation Smoke",160u,120u,true,GraphicsBackend::Vulkan});
        runtime.ResizeViewport(80,60);
        runtime.SetViewportOverlay({{0,0,80,60,{1.f,1.f,0.f}}});
        runtime.DrawFrame();
        // Replace the sampled viewport while keeping the same swapchain.
        runtime.ResizeViewport(120,80);
        runtime.SetViewportOverlay({
            {0,0,120,80,{1.f,0.f,0.f}},
            {0,0,60,40,{0.f,1.f,0.f}},
            {60,40,60,40,{0.f,0.f,1.f}}});
        for(unsigned i=0;i<4;++i){runtime.NativeWindow().PollEvents();runtime.DrawFrame();}
        std::cout<<"PRESENTATION_READY"<<std::endl;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while(!std::filesystem::exists(done)&&std::chrono::steady_clock::now()<deadline)
        {
            runtime.NativeWindow().PollEvents();runtime.DrawFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if(!std::filesystem::exists(done))throw std::runtime_error("Visible window validation timed out.");
        return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
