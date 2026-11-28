#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

import Kairo.Renderer;
import Kairo.Foundation.Math;

int main()
{
    try
    {
        using namespace kairo::renderer;
        using kairo::foundation::math::MakeTranslation;
        using kairo::foundation::math::Vec3f;
        WindowDesc window{"Metal instancing smoke",96u,96u,false};
        window.Backend=GraphicsBackend::Metal;
        RendererRuntime runtime(window);
        if(runtime.Backend()!=GraphicsBackend::Metal)
            throw std::runtime_error("Expected native Metal backend");
        runtime.ResizeViewport(96u,96u);
        CameraPose camera;
        camera.Position={0.f,0.f,5.f};
        camera.Target={};
        camera.Projection=CameraProjectionMode::Orthographic;
        camera.OrthographicSize=4.f;
        runtime.SetCameraPose(camera);
        runtime.SetViewportShadingMode(ViewportShadingMode::Unlit);
        // An actual unlit quad must preserve its base color in the linear
        // RGBA16F capture, even with nonzero scene exposure.
        // A relative image comparison cannot catch a shared tone-map error.
        const auto flat=runtime.CreateMesh(Mesh(
            {{{-1.f,1.f,0.f},{1.f,1.f,1.f},{0.f,0.f,1.f},{0.f,0.f}},
             {{-1.f,-1.f,0.f},{1.f,1.f,1.f},{0.f,0.f,1.f},{0.f,1.f}},
             {{1.f,-1.f,0.f},{1.f,1.f,1.f},{0.f,0.f,1.f},{1.f,1.f}},
             {{1.f,1.f,0.f},{1.f,1.f,1.f},{0.f,0.f,1.f},{1.f,0.f}}},
            {0,1,2,0,2,3}));
        RenderScene flatScene;
        RenderEnvironment flatEnvironment;
        flatEnvironment.ExposureEV100=2.f;
        flatScene.SetEnvironment(flatEnvironment);
        MeshDraw flatDraw;
        flatDraw.Mesh=flat;flatDraw.ObjectID=0x5A17u;
        flatDraw.Material.BaseColor={64.f/255.f,128.f/255.f,192.f/255.f};
        flatDraw.Material.DoubleSided=true;flatDraw.CastShadows=false;
        flatScene.Add(flatDraw);
        runtime.SubmitRenderScene(flatScene);
        runtime.RequestViewportCapture();
        runtime.RequestViewportPick(48,48);
        runtime.DrawFrame();
        const auto flatImage=runtime.TakeViewportCapture();
        const auto flatID=runtime.TakeViewportPickResult();
        if(!flatImage||flatImage->Width!=96||flatImage->Height!=96||!flatID||*flatID!=0x5A17u)
            throw std::runtime_error("Metal unlit quad capture/pick unavailable");
        const auto center=(48u*96u+48u)*4u;
        for(unsigned channel=0;channel<3;++channel)
            if(std::abs(int(flatImage->RGBA.at(center+channel))-int(64u*(channel+1)))>2)
                throw std::runtime_error("Metal unlit color differs from authored linear RGB");
        runtime.SubmitRenderScene(RenderScene{});
        runtime.DestroyMesh(flat);
        std::cout<<"Metal unlit authored color and object ID passed\n";
        const auto geometry=Mesh::MakeCube();
        const auto shared=runtime.CreateMesh(geometry), separate=runtime.CreateMesh(geometry);
        constexpr unsigned left=0x12345678u,right=0xDEADBEEFu;
        const auto capture=[&](bool instanced)
        {
            RenderScene scene;
            MeshDraw draw;
            draw.Mesh=shared; draw.Model=MakeTranslation(Vec3f{-1.f,0.f,0.f});
            draw.ObjectID=left; draw.CastShadows=false;
            scene.Add(draw);
            draw.Mesh=instanced?shared:separate;
            draw.Model=MakeTranslation(Vec3f{1.f,0.f,0.f});
            draw.ObjectID=right;draw.ReceiveShadows=false;
            scene.Add(draw);
            runtime.SubmitRenderScene(scene);
            runtime.RequestViewportCapture();
            runtime.DrawFrame();
            auto image=runtime.TakeViewportCapture();
            if(!image || !image->IsVisuallyNonUniform())
                throw std::runtime_error("Metal image was not captured");
            return *image;
        };
        const auto baseline=capture(false), batched=capture(true);
        if(baseline.Width!=batched.Width || baseline.Height!=batched.Height ||
           baseline.RGBA.size()!=batched.RGBA.size())
            throw std::runtime_error("Instanced image dimensions changed");
        for(std::size_t i=0;i<baseline.RGBA.size();++i)
            if(std::abs(static_cast<int>(baseline.RGBA[i])-static_cast<int>(batched.RGBA[i]))>1)
                throw std::runtime_error("Metal instancing changed visible image");
        const auto pick=[&](unsigned x)
        {
            runtime.RequestViewportPick(x,48);
            runtime.DrawFrame();
            const auto id=runtime.TakeViewportPickResult();
            if(!id)throw std::runtime_error("Missing Metal pick result");
            return *id;
        };
        if(pick(24)!=left || pick(72)!=right)
            throw std::runtime_error("Metal instancing lost per-instance IDs");
        if(capture(false).RGBA!=baseline.RGBA)
            throw std::runtime_error("Ordinary path changed after Metal instancing");
        if(!runtime.SupportsViewportOverlay())throw std::runtime_error("Metal overlay capability missing");
        runtime.SetViewportOverlay({
            {-4,-3,12,11,{1.f,0.f,0.f}},
            {20,44,8,8,{0.f,1.f,0.f}},
            {22,46,4,4,{0.f,0.f,1.f}},
            {100,100,10,10,{1.f,1.f,1.f}}
        });
        const auto ui=capture(false);
        const auto pixel=[&](unsigned x,unsigned y,unsigned r,unsigned g,unsigned b)
        {
            const auto at=(std::size_t(y)*ui.Width+x)*4;
            return ui.RGBA[at]==r&&ui.RGBA[at+1]==g&&ui.RGBA[at+2]==b;
        };
        if(!pixel(0,0,255,0,0)||!pixel(7,7,255,0,0)||
            !pixel(20,44,0,255,0)||!pixel(24,48,0,0,255))
            throw std::runtime_error("Metal viewport overlay clipping or order differs");
        for(unsigned y=0;y<96;++y)for(unsigned x=0;x<96;++x)
        {
            if((x<8&&y<8)||(x>=20&&x<28&&y>=44&&y<52))continue;
            const auto at=(std::size_t(y)*96+x)*4;
            for(unsigned k=0;k<4;++k)if(ui.RGBA[at+k]!=baseline.RGBA[at+k])
                throw std::runtime_error("Metal viewport overlay changed outside its rectangles");
        }
        if(pick(24)!=left)throw std::runtime_error("Metal overlay changed picking");
        bool rejected=false;
        try{runtime.SetViewportOverlay({{0,0,1,1,{2.f,0.f,0.f}}});}
        catch(const std::invalid_argument&){rejected=true;}
        if(!rejected||capture(false).RGBA!=ui.RGBA)
            throw std::runtime_error("Invalid Metal overlay changed previous submission");
        runtime.SetViewportOverlay({});
        if(capture(false).RGBA!=baseline.RGBA)throw std::runtime_error("Removing Metal overlay changed scene");
        std::cout<<"Native Metal depth-independent viewport UI, clipping, painter order and picking passed\n";
        // More than 4 KiB of line vertices; draw across the front faces,
        // verify color changes but the underlying object remains pickable.
        DebugDrawList debug;
        for(unsigned i=0;i<256;++i)
        {
            const float y=-.9f+1.8f*static_cast<float>(i)/255.f;
            debug.AddLine(Vec3f{-1.8f,y,1.5f},Vec3f{1.8f,y,1.5f},
                DebugColor{1.f,1.f,1.f,1.f});
        }
        runtime.SubmitDebugDraw(debug);
        runtime.RequestViewportCapture();
        runtime.DrawFrame();
        auto withDebug=runtime.TakeViewportCapture();
        if(!withDebug || withDebug->RGBA==baseline.RGBA)
            throw std::runtime_error("Large Metal debug batch did not alter rendered pixels");
        if(pick(24)!=left || pick(72)!=right)
            throw std::runtime_error("Debug rendering changed scene object IDs");
        debug.Clear();
        runtime.SubmitDebugDraw(debug);
        if(capture(false).RGBA!=baseline.RGBA)
            throw std::runtime_error("Debug geometry persisted after clearing");
        // Add offscreen geometry between otherwise compatible draws. It
        // must not change the visible image or disappear permanently when
        // the camera moves. Preserve its full picking ID.
        RenderScene sparse;
        MeshDraw draw;
        draw.Mesh=shared; draw.CastShadows=false;
        draw.Model=MakeTranslation(Vec3f{-1.f,0.f,0.f}); draw.ObjectID=left;
        sparse.Add(draw);
        draw.Model=MakeTranslation(Vec3f{20.f,0.f,0.f}); draw.ObjectID=0xCAFEBABEu;
        sparse.Add(draw);
        draw.Model=MakeTranslation(Vec3f{1.f,0.f,0.f}); draw.ObjectID=right;
        draw.ReceiveShadows=false; sparse.Add(draw);
        runtime.SubmitRenderScene(sparse);
        runtime.RequestViewportCapture();
        runtime.DrawFrame();
        const auto sparseCapture=runtime.TakeViewportCapture();
        if(!sparseCapture || sparseCapture->RGBA!=baseline.RGBA)
            throw std::runtime_error("Metal camera culling changed visible image");
        auto movedCamera=camera;
        movedCamera.Position.x=20.f; movedCamera.Target.x=20.f;
        runtime.SetCameraPose(movedCamera);
        if(pick(48)!=0xCAFEBABEu)
            throw std::runtime_error("Metal camera visibility did not refresh");
        runtime.SetCameraPose(camera);
        if(pick(24)!=left || pick(72)!=right)
            throw std::runtime_error("Metal camera return lost original IDs");
        // Both casters are outside the camera's horizontal clip planes, but
        // the directional light sees them. Compare ordinary and instanced
        // shadow commands, then prove the offscreen casters affected pixels.
        runtime.SetViewportShadingMode(ViewportShadingMode::Lit);
        const auto captureShadow = [&](bool instanced, bool cast)
        {
            RenderScene scene;
            MeshDraw caster;
            caster.Mesh = shared;
            caster.Model = MakeTranslation(Vec3f{ 3.2f, 0.0f, 2.2f });
            caster.CastShadows = cast;
            scene.Add(caster);
            caster.Mesh = instanced ? shared : separate;
            caster.Model = MakeTranslation(Vec3f{ 3.2f, 0.0f, 3.4f });
            scene.Add(caster);
            MeshDraw receiver;
            receiver.Mesh = shared;
            receiver.CastShadows = false;
            scene.Add(receiver);
            RenderLight light;
            light.Type = RenderLightType::Directional;
            light.Direction = { 1.0f, 0.1f, 1.0f };
            light.Intensity = 3.0f;
            light.CastShadows = true;
            scene.AddLight(light);
            runtime.SubmitRenderScene(scene);
            runtime.RequestViewportCapture();
            runtime.DrawFrame();
            auto image = runtime.TakeViewportCapture();
            if (!image || !image->IsVisuallyNonUniform())
                throw std::runtime_error("Missing shadow comparison image");
            return *image;
        };
        const auto ordinaryShadow = captureShadow(false, true);
        const auto instancedShadow = captureShadow(true, true);
        const auto noShadow = captureShadow(true, false);
        if (ordinaryShadow.RGBA.size() != instancedShadow.RGBA.size())
            throw std::runtime_error("Instanced shadow image dimensions differ");
        for (std::size_t i = 0; i < ordinaryShadow.RGBA.size(); ++i)
            if (std::abs(static_cast<int>(ordinaryShadow.RGBA[i]) -
                static_cast<int>(instancedShadow.RGBA[i])) > 1)
                throw std::runtime_error("Instanced shadow differs from ordinary reference");
        if (ordinaryShadow.RGBA == noShadow.RGBA)
            throw std::runtime_error("Offscreen shadow casters had no visible effect");

        std::cout<<"Metal native instancing, picking, debug, camera visibility and offscreen shadows passed\n";
        return 0;
    }
    catch(const kairo::renderer::PresentationUnavailableError& e)
    {
        std::cerr<<"Metal presentation unavailable: "<<e.what()<<'\n';
        return 77;
    }
    catch(const std::exception& e)
    {
        std::cerr<<"Metal instancing smoke failed: "<<e.what()<<'\n';
        return 1;
    }
}
