# Teaching source map — KairoRenderer

> A guided source-reading itinerary. It is not a test result or claim that every file is documented yet.

## How to use this guide

Work from backend-neutral types and graph policies toward API-specific execution. Read Vulkan first for the explicit resource/synchronization path, then compare Metal, OpenGL and D3D12 adapters.

Read each phase in order. Every link goes to a source file or directory in this repository. For a directory, inspect its public interface and then its implementation files. After each phase, identify the caller, the state it owns, the inputs it validates, and its observable outputs.

## 1. API and scene contracts

1. [`README.md`](README.md)
2. [`KairoRenderer.cppm`](KairoRenderer.cppm)
3. [`RendererTypes.cppm`](RendererTypes.cppm)
4. [`GraphicsBackend.cppm`](GraphicsBackend.cppm)
5. [`Window.cppm`](Window.cppm)
6. [`Camera.cppm`](Camera.cppm)
7. [`RenderScene.cppm`](RenderScene.cppm)
8. [`Mesh.cppm`](Mesh.cppm)
9. [`Material.cppm`](Material.cppm)
10. [`Texture.cppm`](Texture.cppm)

## 2. Rendering algorithms

1. [`RenderGraph.cppm`](RenderGraph.cppm)
2. [`ShadowSettings.cppm`](ShadowSettings.cppm)
3. [`Skinning.cppm`](Skinning.cppm)
4. [`DebugDraw.cppm`](DebugDraw.cppm)
5. [`AssetAdapters.cppm`](AssetAdapters.cppm)
6. [`PhysicsDebugBridge.cppm`](PhysicsDebugBridge.cppm)

## 3. Backends

1. [`Renderer.cppm`](Renderer.cppm)
2. [`VulkanInstance.cppm`](VulkanInstance.cppm)
3. [`VulkanDevice.cppm`](VulkanDevice.cppm)
4. [`VulkanSwapchain.cppm`](VulkanSwapchain.cppm)
5. [`VulkanSync.cppm`](VulkanSync.cppm)
6. [`VulkanDescriptor.cppm`](VulkanDescriptor.cppm)
7. [`VulkanRenderer.cppm`](VulkanRenderer.cppm)
8. [`MetalRenderer.cppm`](MetalRenderer.cppm)
9. [`OpenGLRenderer.cppm`](OpenGLRenderer.cppm)
10. [`Direct3D12Renderer.cppm`](Direct3D12Renderer.cppm)

## 4. Tests

1. [`tests/RendererTests.cpp`](tests/RendererTests.cpp)
2. [`tests/RenderGraphExecutionPlanTests.cpp`](tests/RenderGraphExecutionPlanTests.cpp)
3. [`tests/SkinningContractTests.cpp`](tests/SkinningContractTests.cpp)
4. [`tests/VulkanSkinningSmoke.cpp`](tests/VulkanSkinningSmoke.cpp)
5. [`tests/MetalSkinningSmoke.cpp`](tests/MetalSkinningSmoke.cpp)
6. [`tests/OpenGLSkinningSmoke.cpp`](tests/OpenGLSkinningSmoke.cpp)
7. [`tests/Direct3D12SkinningSmoke.cpp`](tests/Direct3D12SkinningSmoke.cpp)

## End-to-end interaction map

`RendererRuntime` is a façade dispatching a common scene/render API to a compiled backend. KairoAssets produces canonical mesh/texture artifacts, KairoEngineCore supplies authored scene state, and KairoEditor/Player render through the same bridge. The render graph models pass order and lifetimes; backend implementations own native device resources and presentation.

## APIs and code-reading questions

For each function, distinguish **contract** (preconditions, postconditions, failure modes), **implementation** (state/algorithms), **collaborators** (imports, calls and consumers), **lifetime/ownership**, and **verification** (test or evidence that checks it). For C++ modules, `export module` names the public compiled interface and `import` expresses module dependencies; an implementation unit's `module ...;` contributes definitions without exporting them.

## Boundaries and honesty

A successful cross-platform policy test does not equal native execution on every OS. D3D12 requires a real Windows gate, and native pass scheduling still has deferred work.

## Documentation rules

- Teaching comments describe *why*, invariants, edge cases, API ownership and cross-file effects; avoid narrating obvious assignments.
- Preserve behavior during documentation-only edits; keep correctness changes in separate reviewed commits.
- Link function names and tests where possible; mark claims as source-present, CI-observed, host-verified or research-evidenced.
- License grants apply only to material for which the repository owner holds rights; third-party source, models, media and datasets keep their own notices.
