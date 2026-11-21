# KairoRenderer — teaching order and API ownership

Read the renderer in **layers**, not alphabetically: configuration → scene contracts → render graph → backend-neutral facade → one complete native backend → other backends → parity tests. KairoRenderer is a sibling of [KairoAssets](https://github.com/swayam8624/KairoAssets) and [KairoEngineCore](https://github.com/swayam8624/KairoEngineCore); the Player and Editor use its public APIs through the [KairoGameEngine integration bridges](https://github.com/swayam8624/KairoGameEngine).

```text
Authored project -> KairoAssets -> shared scene/mesh/material adapter
                                             |
                                   Kairo.Renderer API
                                             |
                              validated RenderGraph/pass plan
                                             |
          +------------------+---------------+-----------------+
          |                  |               |                 |
     Vulkan runtime       Metal runtime    D3D12 runtime    OpenGL runtime
          |                  |               |                 |
     Device/sync         MTL buffers        DXGI/D3D12     GL context
          +------------------+---------------+-----------------+
                                      |
                             GPU scene + picking + readback
```

## Follow this source order

1. [`KairoRenderer.cppm`](KairoRenderer.cppm) — umbrella `export module Kairo.Renderer`, exporting the public component surface via `export import`. One import does not mean one monolithic implementation.
2. [`GraphicsBackend.cppm`](GraphicsBackend.cppm) — backend selection, compiled availability, explicit failure and host preference. Synthetic platform policy tests are not actual Windows/macOS/Linux native backend execution.
3. [`RendererTypes.cppm`](RendererTypes.cppm), [`Window.cppm`](Window.cppm), and [`Camera.cppm`](Camera.cppm) — frame, window and camera data contracts independent of a specific graphics API.
4. [`Mesh.cppm`](Mesh.cppm), [`Texture.cppm`](Texture.cppm), [`Material.cppm`](Material.cppm), [`Skinning.cppm`](Skinning.cppm) — model geometry, vertex attributes, textures, physically based materials, and animation/skinning data. Know where sRGB and linear spaces differ.
5. [`RenderScene.cppm`](RenderScene.cppm), [`AssetAdapters.cppm`](AssetAdapters.cppm), and [`ShadowSettings.cppm`](ShadowSettings.cppm) — authored content to draw instances, lights, shadow configuration and resource validation.
6. [`RenderGraph.cppm`](RenderGraph.cppm) — passes, resources, access states, lifetimes, dependency ordering and CPU profiling. A graph schedule is not automatically a complete per-backend GPU barrier implementation.
7. [`Renderer.cppm`](Renderer.cppm), [`DebugDraw.cppm`](DebugDraw.cppm), and [`PhysicsDebugBridge.cppm`](PhysicsDebugBridge.cppm) — runtime-facing rendering facade, developer visualization and physics debug extraction.

### Native Vulkan path: follow actual device ownership

8. [`VulkanInstance.cppm`](VulkanInstance.cppm), [`VulkanSurface.cppm`](VulkanSurface.cppm), [`VulkanDevice.cppm`](VulkanDevice.cppm) — loader, surface, queue family and device lifetime.
9. [`VulkanSwapchain.cppm`](VulkanSwapchain.cppm), [`VulkanSync.cppm`](VulkanSync.cppm), [`VulkanCommand.cppm`](VulkanCommand.cppm) — acquire/submit/present, fences/semaphores and command recording. Do not treat GPU completion and CPU submission as identical.
10. [`VulkanBuffer.cppm`](VulkanBuffer.cppm), [`VulkanDescriptor.cppm`](VulkanDescriptor.cppm), [`VulkanTexture.cppm`](VulkanTexture.cppm), [`VulkanDepth.cppm`](VulkanDepth.cppm), [`VulkanShadowMap.cppm`](VulkanShadowMap.cppm) — storage, descriptors, image state, depth and shadow reads.
11. [`VulkanTriangle.cppm`](VulkanTriangle.cppm), [`VulkanRenderer.cppm`](VulkanRenderer.cppm), [`VulkanViewportTarget.cppm`](VulkanViewportTarget.cppm) — actual draw path, render-graph integration, viewport, picking/readback and presentation.

### Other native backends

12. [`MetalRenderer.cppm`](MetalRenderer.cppm) and [`detail/MetalBackend.mm`](detail/MetalBackend.mm) — public Metal runtime versus Objective-C++ resource ownership and CAMetalLayer presentation.
13. [`Direct3D12Renderer.cppm`](Direct3D12Renderer.cppm) and [`detail/Direct3D12Backend.cpp`](detail/Direct3D12Backend.cpp) — Windows-gated D3D12 resource/swapchain path; Windows CI/native execution must verify it.
14. [`OpenGLRenderer.cppm`](OpenGLRenderer.cppm) and [`OpenGLShaders.cppm`](OpenGLShaders.cppm) — OpenGL context, render state, shaders and projection differences.

### Verify public contracts

15. [`tests/RendererTests.cpp`](tests/RendererTests.cpp), [`tests/RenderGraphExecutionPlanTests.cpp`](tests/RenderGraphExecutionPlanTests.cpp) and [`tests/RenderGraphScaleTests.cpp`](tests/RenderGraphScaleTests.cpp) — contract and graph tests.
16. [`tests/VulkanSkinningSmoke.cpp`](tests/VulkanSkinningSmoke.cpp), [`tests/MetalSkinningSmoke.cpp`](tests/MetalSkinningSmoke.cpp), [`tests/Direct3D12SkinningSmoke.cpp`](tests/Direct3D12SkinningSmoke.cpp), [`tests/OpenGLSkinningSmoke.cpp`](tests/OpenGLSkinningSmoke.cpp) — host-specific paths; a compile test is not necessarily a device/presentation success.
17. [`CMakeLists.txt`](CMakeLists.txt) — module/build graph, platform flags and backend selection. [`STATUS.md`](STATUS.md) still calls the 95% completion target unverified.

## API glossary

**`GraphicsBackend`** is a user selection policy, **not** an already-created GPU device. **`RenderScene`** is portable scene data; **`RenderGraph`** expresses dependencies/resource states; **`VulkanRenderer` / `MetalRenderer` / others** implement real graphics API commands. **`AssetAdapters`** convert validated portable assets; they do not transfer ownership of persistent source assets to the GPU. **`DebugDraw`** is instrumentation, not a physics solver.

Learn by tracing one imported glTF mesh from KairoAssets to the scene, material descriptors, GPU vertex/index resources, render pass, object-ID target and readback. Compare the result on a native backend and on the other supported hosts **only where those hosts were actually run**. Code is shared under [MIT](LICENSE) for original contributions; third-party Vulkan/Metal/GLFW/SDK dependencies have their own licenses.
