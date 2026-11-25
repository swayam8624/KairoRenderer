#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

import Kairo.Renderer;
import Kairo.Foundation.Math;

// Compare native instancing with separate but identical mesh allocations,
// which forces the ordinary draw path without changing geometry or material.
// The high-bit picking IDs must survive the integer instance attribute.
int main()
{
    try
    {
        using namespace kairo::renderer;
        using kairo::foundation::math::MakeTranslation;
        using kairo::foundation::math::Vec3f;
        WindowDesc window{ "Vulkan instancing smoke", 96u, 96u, false };
        window.Backend = GraphicsBackend::Vulkan;
        RendererRuntime runtime(window);
        runtime.ResizeViewport(96u, 96u);
        CameraPose camera;
        camera.Position = { 0.0f, 0.0f, 5.0f };
        camera.Target = {};
        camera.Projection = CameraProjectionMode::Orthographic;
        camera.OrthographicSize = 4.0f;
        runtime.SetCameraPose(camera);
        runtime.SetViewportShadingMode(ViewportShadingMode::Unlit);
        const auto geometry = Mesh::MakeCube();
        const auto shared = runtime.CreateMesh(geometry);
        const auto separate = runtime.CreateMesh(geometry);
        constexpr unsigned leftID = 0x12345678u, rightID = 0xDEADBEEFu;

        const auto makeScene = [&](bool instanced)
        {
            RenderScene scene;
            MeshDraw draw;
            draw.Mesh = shared;
            draw.Model = MakeTranslation(Vec3f{ -1.0f, 0.0f, 0.0f });
            draw.ObjectID = leftID;
            draw.CastShadows = false;
            scene.Add(draw);
            draw.Mesh = instanced ? shared : separate;
            draw.Model = MakeTranslation(Vec3f{ 1.0f, 0.0f, 0.0f });
            draw.ObjectID = rightID;
            draw.ReceiveShadows = false;
            scene.Add(draw);
            return scene;
        };

        const auto render = [&](bool instanced)
        {
            runtime.SubmitRenderScene(makeScene(instanced));
            runtime.RequestViewportCapture();
            runtime.DrawFrame();
            runtime.DrawFrame();
            auto image = runtime.TakeViewportCapture();
            if (!image || !image->IsVisuallyNonUniform())
                throw std::runtime_error("Expected visible Vulkan instance geometry");
            return *image;
        };

        const auto baseline = render(false);
        const auto batched = render(true);
        if (baseline.Width != batched.Width || baseline.Height != batched.Height ||
            baseline.RGBA.size() != batched.RGBA.size())
            throw std::runtime_error("Instanced output dimensions differ");
        for (std::size_t i = 0u; i < baseline.RGBA.size(); ++i)
            if (std::abs(static_cast<int>(baseline.RGBA[i]) - static_cast<int>(batched.RGBA[i])) > 1)
                throw std::runtime_error("Instanced image differs from ordinary draw reference");

        const auto pick = [&](unsigned x)
        {
            runtime.RequestViewportPick(x, 48u);
            runtime.DrawFrame();
            runtime.DrawFrame();
            const auto id = runtime.TakeViewportPickResult();
            if (!id) throw std::runtime_error("Missing Vulkan instance picking result");
            return *id;
        };
        if (pick(24u) != leftID || pick(72u) != rightID)
            throw std::runtime_error("Instanced draws lost individual picking identity");

        // Screen-space UI must overwrite world color without disturbing ID,
        // obey top-left clipping and painter order, and restore exactly.
        runtime.SetVulkanViewportOverlay({
            {-4,-3,12,11,{1.f,0.f,0.f}},
            {20,44,8,8,{0.f,1.f,0.f}},
            {22,46,4,4,{0.f,0.f,1.f}},
            {100,100,10,10,{1.f,1.f,1.f}}
        });
        const auto ui=render(false);
        const auto pixel=[&](unsigned x,unsigned y,unsigned r,unsigned g,unsigned b)
        {
            const auto at=(std::size_t(y)*ui.Width+x)*4;
            return ui.RGBA[at]==r&&ui.RGBA[at+1]==g&&ui.RGBA[at+2]==b;
        };
        if(!pixel(0,0,255,0,0)||!pixel(7,7,255,0,0)||
            !pixel(20,44,0,255,0)||!pixel(24,48,0,0,255))
            throw std::runtime_error("Viewport overlay clipping or order differs");
        for(unsigned y=0;y<96;++y)for(unsigned x=0;x<96;++x)
        {
            if((x<8&&y<8)||(x>=20&&x<28&&y>=44&&y<52))continue;
            const auto at=(std::size_t(y)*96+x)*4;
            for(unsigned k=0;k<4;++k)if(ui.RGBA[at+k]!=baseline.RGBA[at+k])
                throw std::runtime_error("Viewport overlay changed outside its rectangles");
        }
        if(pick(24u)!=leftID)throw std::runtime_error("Viewport overlay changed picking identity");
        bool rejected=false;
        try{runtime.SetVulkanViewportOverlay({{0,0,1,1,{2.f,0.f,0.f}}});}
        catch(const std::invalid_argument&){rejected=true;}
        if(!rejected||render(false).RGBA!=ui.RGBA)
            throw std::runtime_error("Invalid overlay changed previous submission");
        runtime.SetVulkanViewportOverlay({});
        if(render(false).RGBA!=baseline.RGBA)
            throw std::runtime_error("Removing viewport overlay changed scene");
        std::cout<<"Native depth-independent viewport UI, clipping, painter order and picking passed\n";

        // Returning to ordinary draws must not leave instance bindings active
        // or otherwise corrupt the original pipeline.
        const auto restored = render(false);
        if (restored.RGBA != baseline.RGBA)
            throw std::runtime_error("Ordinary draw fallback changed after instancing");

        // Exceed the static hierarchy threshold with invisible geometry.
        // Two frames build the BVH; compare against the small ordinary scene.
        const auto largeScene = [&](bool moved)
        {
            auto scene = makeScene(false);
            MeshDraw extra;
            extra.Mesh = shared;
            extra.CastShadows = false;
            extra.ObjectID = 0xCAFEBABEu;
            extra.Model = MakeTranslation(moved ? Vec3f{0.0f,0.0f,2.0f} :
                Vec3f{20.0f,0.0f,0.0f});
            scene.Add(extra);
            for (unsigned i = 0u; i < 256u; ++i)
            {
                extra.ObjectID = 1000u + i;
                extra.Model = MakeTranslation(Vec3f{100.0f + static_cast<float>(i)*3.0f,0.0f,0.0f});
                scene.Add(extra);
            }
            return scene;
        };
        runtime.SubmitRenderScene(largeScene(false));
        runtime.DrawFrame();
        runtime.DrawFrame();
        runtime.RequestViewportCapture();
        runtime.DrawFrame();
        runtime.DrawFrame();
        const auto largeCapture = runtime.TakeViewportCapture();
        if (!largeCapture || largeCapture->RGBA != baseline.RGBA)
            throw std::runtime_error("Static visibility hierarchy changed visible image");

        auto movedCamera = camera;
        movedCamera.Position.x = 20.0f;
        movedCamera.Target.x = 20.0f;
        runtime.SetCameraPose(movedCamera);
        if (pick(48u) != 0xCAFEBABEu)
            throw std::runtime_error("Camera change reused stale visibility");
        runtime.SetCameraPose(camera);
        runtime.SubmitRenderScene(largeScene(true));
        if (pick(48u) != 0xCAFEBABEu)
            throw std::runtime_error("Moved object failed to invalidate static visibility hierarchy");

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

        // Destroy after submission, before the next frame fence wait.
        // Public handles must become invalid immediately, although their
        // native buffers are still retained for the preceding command.
        runtime.SetViewportShadingMode(ViewportShadingMode::Unlit);
        runtime.SubmitRenderScene(makeScene(false));
        runtime.DrawFrame();
        runtime.DestroyMesh(separate);
        bool rejected=false;
        try { runtime.SubmitRenderScene(makeScene(false)); }
        catch(const std::out_of_range&) { rejected=true; }
        if(!rejected) throw std::runtime_error("Retired mesh handle was accepted");
        if(pick(24u)!=leftID || pick(72u)==rightID)
            throw std::runtime_error("Retirement changed surviving draw or retained deleted draw");
        runtime.DestroyMesh(shared);
        runtime.SubmitRenderScene(RenderScene{});
        // No DrawFrame between removals: force the bounded journal's
        // idle fallback and verify the next frame still succeeds.
        for(unsigned i=0;i<257u;++i)
            runtime.DestroyMesh(runtime.CreateMesh(geometry));
        runtime.DrawFrame();
        runtime.DrawFrame();
        std::cout << "Vulkan instancing, picking, shadows and fence-retired meshes passed\n";
        return 0;
    }
    catch (const kairo::renderer::PresentationUnavailableError& error)
    {
        std::cerr << "Vulkan presentation unavailable: " << error.what() << '\n';
        return 77;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Vulkan instancing smoke failed: " << error.what() << '\n';
        return 1;
    }
}
