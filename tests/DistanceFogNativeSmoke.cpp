#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

import Kairo.Renderer;
import Kairo.Foundation.Math;

int main(int argc,char** argv)
{
    try
    {
        using namespace kairo::renderer;
        using kairo::foundation::math::MakeTranslation;
        using kairo::foundation::math::Vec3f;
        if(argc!=2)throw std::invalid_argument("Expected native backend name.");
        const std::string_view backendName=argv[1];
        if(backendName!="metal"&&backendName!="vulkan")throw std::invalid_argument("Expected Vulkan or Metal.");
        const auto backend=backendName=="metal"?GraphicsBackend::Metal:GraphicsBackend::Vulkan;
        RendererRuntime runtime({"Native distance fog",96u,96u,false,backend});
        if(runtime.Backend()!=backend||!runtime.SupportsDistanceFog())throw std::runtime_error("Native fog capability missing.");
        runtime.ResizeViewport(96,96);
        CameraPose camera;camera.Position={0.f,0.f,5.f};camera.Target={};
        camera.Projection=CameraProjectionMode::Orthographic;camera.OrthographicSize=4.f;
        runtime.SetCameraPose(camera);
        const auto mesh=runtime.CreateMesh(Mesh::MakeCube());
        RenderScene scene;MeshDraw draw;draw.Mesh=mesh;draw.CastShadows=false;draw.ReceiveShadows=false;
        draw.Material.BaseColor={1.f,.1f,.1f};draw.Material.Emissive={2.f,1.f,.5f};
        draw.Model=MakeTranslation(Vec3f{-1.f,0.f,0.f});draw.ObjectID=17u;scene.Add(draw);
        draw.Model=MakeTranslation(Vec3f{1.f,0.f,-4.f});draw.ObjectID=29u;scene.Add(draw);
        // Lighting diagnostics need a direct light to distinguish geometry from clear.
        RenderLight light;
        light.Direction={0.f,0.f,1.f};
        light.CastShadows=false;
        scene.AddLight(light);
        RenderEnvironment environment;environment.BackgroundColor={0.f,0.f,0.f};
        environment.AmbientIntensity=1.f;environment.EnvironmentIntensity=0.f;environment.FogStartDistance=6.f;
        const auto capture=[&](float density)
        {
            environment.FogDensity=density;scene.SetEnvironment(environment);runtime.SubmitRenderScene(scene);
            runtime.RequestViewportCapture();
            for(unsigned i=0;i<16;++i)
            {
                runtime.DrawFrame();
                if(auto image=runtime.TakeViewportCapture())
                {
                    if(!image->IsVisuallyNonUniform()||image->Width!=96||image->Height!=96)
                        throw std::runtime_error("Invalid native fog capture.");
                    const auto& passes=runtime.LastFrameProfile().Passes;
                    const auto has=[&](std::string_view name){return std::any_of(passes.begin(),passes.end(),[&](const auto& p){return p.Name==name;});};
                    if(backend==GraphicsBackend::Vulkan?(!has("Record")||!has("Submit")):(!has("RenderPresent")||!has("CaptureReadback")))
                        throw std::runtime_error("Native render graph profile missing.");
                    return *image;
                }
            }
            throw std::runtime_error("Native fog capture unavailable.");
        };
        const auto pixel=[](const ViewportCapture& image,unsigned x,unsigned y)
        {
            const auto at=(static_cast<std::size_t>(y)*image.Width+x)*4;
            return std::array<int,3>{image.RGBA.at(at),image.RGBA.at(at+1),image.RGBA.at(at+2)};
        };
        const auto pick=[&](unsigned x)
        {
            runtime.RequestViewportPick(x,48);
            for(unsigned i=0;i<16;++i){runtime.DrawFrame();if(auto id=runtime.TakeViewportPickResult())return *id;}
            throw std::runtime_error("Fog pick result unavailable.");
        };
        runtime.SetViewportShadingMode(ViewportShadingMode::Lit);
        const auto baseline=capture(0.f),fogged=capture(.4f);
        const auto near0=pixel(baseline,24,48),near1=pixel(fogged,24,48);
        const auto far0=pixel(baseline,72,48),far1=pixel(fogged,72,48);
        for(unsigned channel=0;channel<3;++channel)
            if(std::abs(near0[channel]-near1[channel])>1)throw std::runtime_error("Near geometry changed before fog start.");
        if(far0[0]-far1[0]<10||baseline.RGBA==fogged.RGBA||pixel(baseline,0,0)!=pixel(fogged,0,0))
            throw std::runtime_error("Far geometry or background fog mismatch.");
        if(pick(24)!=17u||pick(72)!=29u)throw std::runtime_error("Fog changed picking.");
        auto invalid=environment;invalid.FogDensity=std::numeric_limits<float>::quiet_NaN();
        bool rejected=false;try{scene.SetEnvironment(invalid);}catch(const std::invalid_argument&){rejected=true;}
        if(!rejected||scene.Environment().FogDensity!=.4f)throw std::runtime_error("Invalid fog replaced authored scene.");
        if(capture(0.f).RGBA!=baseline.RGBA)throw std::runtime_error("Disabling fog failed to restore baseline.");
        for(const auto mode:{ViewportShadingMode::Normals,ViewportShadingMode::Unlit,ViewportShadingMode::Lighting})
        {
            runtime.SetViewportShadingMode(mode);
            if(capture(0.f).RGBA!=capture(.4f).RGBA)throw std::runtime_error("Fog altered diagnostic view.");
        }
        // A linear equation is enabled even with zero exponential density.
        // Far geometry saturates to its own authored color; clear stays black.
        runtime.SetViewportShadingMode(ViewportShadingMode::Lit);
        environment.Fog=RenderFogMode::Linear;
        environment.FogEndDistance=7.f;
        environment.FogColor=Vec3f{.05f,.1f,.9f};
        const auto linear=capture(0.f);
        const auto farLinear=pixel(linear,72,48),nearLinear=pixel(linear,24,48);
        if(farLinear[2]-farLinear[0]<40||pixel(linear,0,0)!=pixel(baseline,0,0))
            throw std::runtime_error("Linear fog did not apply independent color.");
        for(unsigned channel=0;channel<3;++channel)
            if(std::abs(nearLinear[channel]-near0[channel])>1)throw std::runtime_error("Linear near cutoff changed geometry.");
        if(pick(24)!=17u||pick(72)!=29u)throw std::runtime_error("Linear fog changed picking.");
        invalid=environment;invalid.FogEndDistance=invalid.FogStartDistance;
        rejected=false;try{scene.SetEnvironment(invalid);}catch(const std::invalid_argument&){rejected=true;}
        if(!rejected||scene.Environment().FogEndDistance!=7.f)
            throw std::runtime_error("Invalid linear interval replaced scene.");
        environment.Fog=RenderFogMode::Disabled;
        if(capture(1.f).RGBA!=baseline.RGBA)throw std::runtime_error("Explicit disabled mode did not restore baseline.");
        runtime.SubmitRenderScene(RenderScene{});runtime.DestroyMesh(mesh);
        std::cout<<"Native fog: near/far contrast, disabled restoration, picking and diagnostic isolation passed\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
