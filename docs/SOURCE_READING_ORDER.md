# Teaching source map — KairoRenderer

> A guided source-reading itinerary. It is not a test result or claim that every file is documented yet.

## How to use this guide

Work from backend-neutral types and graph policies toward API-specific execution. Read Vulkan first for the explicit resource/synchronization path, then compare Metal, OpenGL and D3D12 adapters.

Read each phase in order. Every link goes to a source file or directory in this repository. For a directory, inspect its public interface and then its implementation files. After each phase, identify the caller, the state it owns, the inputs it validates, and its observable outputs.

**Vulkan descriptor pool safety:** [PBR descriptor preflight, allocation cleanup, and future strong-generation transactions](VULKAN_DESCRIPTOR_REBUILD_ERROR_SAFETY_LAB.md). **CI evidence route:** [`cpu-render-resource-guards.yml`](../.github/workflows/cpu-render-resource-guards.yml) compiles the portable fault-injection and frame-journal tests on GCC/Clang.

**Vulkan failed-frame recovery:** [WSI acquire semaphore draining, late fence reset, queue submission errors and fail-fast device state](VULKAN_FRAME_FAILURE_RECOVERY_LAB.md). Read [`VulkanRenderer.cppm`](../VulkanRenderer.cppm) with the portable [state journal](../detail/VulkanFrameRecoveryJournal.hpp) and [regression tests](../tests/VulkanFrameRecoveryJournalTests.cpp).

**Vulkan constructor exception-safety:** [failed command buffer, mapped allocation, semaphore/fence, swapchain image view and depth-view rollback](VULKAN_PARTIAL_CONSTRUCTION_ROLLBACK_LAB.md). Compare actual constructor catch paths before studying GPU fault-injection limitations.

**Correctness follow-on:** [Metal/D3D12 native handle publication, OpenGL post-upload cleanup, and Player scene retirement](https://github.com/swayam8624/KairoGameEngine/blob/main/docs/GPU_RESOURCE_ROLLBACK_AND_RENDERER_TEARDOWN_LAB.md). Read [`detail/NativeResourcePublication.hpp`](../detail/NativeResourcePublication.hpp) then [`tests/NativeResourcePublicationTests.cpp`](../tests/NativeResourcePublicationTests.cpp), followed by native backend source.

**Source teaching status:** [annotated native files and outstanding correctness/test evidence](SOURCE_TEACHING_PROGRESS.md).

**Cross-subsystem texture laboratory:** [Texture upload, mip offsets, descriptor sets and native viewport/shadow transitions](TEXTURE_UPLOAD_AND_RENDER_TARGET_TRANSITIONS_LAB.md). Read `TextureArtifact.cppm` and its tests before studying GPU image ownership, color-space interpretation and native layout barriers.

**Cross-repository integration lab:** [CPU authored asset-to-GPU handle bridge and shared Editor/Player extraction](https://github.com/swayam8624/KairoGameEngine/blob/main/docs/ASSET_SCENE_TO_GPU_LIFETIME_LAB.md). Read the authored/CPU contracts before following the native GPU backend; source inspection is not a native test result.

**Native synchronization and ownership:** [Vulkan acquire/submit/present fences, Metal command completion, readbacks, native teardown and error-path audits](NATIVE_GPU_SYNCHRONIZATION_AND_LIFETIMES_LAB.md). Start this after the render-graph overview to distinguish logical transitions from actual GPU ordering.

**Algorithm exercise:** [How RenderGraph compiles resource hazards, orders passes and reuses transient storage](RENDER_GRAPH_COMPILER_WORKED_EXAMPLES.md). It also documents declaration-error exception-safety limitations.

## 1. Foundational contracts and resource ownership

The best starting point is the new
[frame-lifecycle walkthrough](FRAME_LIFECYCLE_WALKTHROUGH.md). This guide is
the complete navigation index; the walkthrough connects API calls, ownership
and backend runtime behavior in one example.

1. [`README.md`](../README.md) and [`CMakeLists.txt`](../CMakeLists.txt): milestones, host compiler requirements and native backend build flags.
2. [`KairoRenderer.cppm`](../KairoRenderer.cppm): umbrella exports and CPU-only telemetry meanings.
3. [`GraphicsBackend.cppm`](../GraphicsBackend.cppm): compiled availability, host compatibility and auto-versus-explicit selection.
4. [`RendererTypes.cppm`](../RendererTypes.cppm): shared settings/viewport status.
5. [`Window.cppm`](../Window.cppm): platform window lifetime and backend selection before GPU creation.
6. [`Camera.cppm`](../Camera.cppm): camera pose, projection and matrix convention.
7. [`Mesh.cppm`](../Mesh.cppm): CPU mesh payloads and buffer description.
8. [`Skinning.cppm`](../Skinning.cppm): joint palettes, bounds and vertex/skin contract.
9. [`Material.cppm`](../Material.cppm) and [`Texture.cppm`](../Texture.cppm): PBR channels, formats and sampler conventions.
10. [`RenderScene.cppm`](../RenderScene.cppm): draw/lighting submission and object IDs.

**Checkpoint:** distinguish authored AssetID, CPU artifact, uploaded MeshHandle,
draw-object ID and native GPU allocation. Read RenderScene::Validate and
explain what should fail before recording any graphics commands.

## 2. Cross-repository scene translation and logical scheduling

11. [`AssetAdapters.cppm`](../AssetAdapters.cppm): converting portable KairoAssets artifacts to runtime renderer data.
12. [`ShadowSettings.cppm`](../ShadowSettings.cppm): directional depth and PCF tuning contracts.
13. [`DebugDraw.cppm`](../DebugDraw.cppm): debug line data rather than authoritative game state.
14. [`PhysicsDebugBridge.cppm`](../PhysicsDebugBridge.cppm): read-only PhysicsEngine shape/contact visualization.
15. [`RenderGraph.cppm`](../RenderGraph.cppm): passes, hazard detection, topological order, resource lifetimes and planned alias slots.
16. [`tests/RenderGraphExecutionPlanTests.cpp`](../tests/RenderGraphExecutionPlanTests.cpp) and [`tests/RenderGraphScaleTests.cpp`](../tests/RenderGraphScaleTests.cpp): validate graph behavior and complexity cases.

**Checkpoint:** create three logical passes using a shared resource. Explain
why a read-before-write is invalid; identify the explicit dependency and
logical state transition. These are CPU scheduling contracts, not automatic
native GPU barriers.

## 3. Runtime facade and explicit Vulkan backend

17. [`Renderer.cppm`](../Renderer.cppm): RendererRuntime facade, std::variant selection and DrawFrame envelope.
18. [`VulkanInstance.cppm`](../VulkanInstance.cppm): VkInstance extension and validation lifetime.
19. [`VulkanSurface.cppm`](../VulkanSurface.cppm): window/native surface contract.
20. [`VulkanDevice.cppm`](../VulkanDevice.cppm): physical/logical device and queue selection.
21. [`VulkanSwapchain.cppm`](../VulkanSwapchain.cppm): image format, extent, acquisition and presentation.
22. [`VulkanCommand.cppm`](../VulkanCommand.cppm): command recording/reuse.
23. [`VulkanSync.cppm`](../VulkanSync.cppm): fences, semaphores and frame reuse.
24. [`VulkanBuffer.cppm`](../VulkanBuffer.cppm), [`VulkanTexture.cppm`](../VulkanTexture.cppm) and [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm): upload, lifetime, format/layout and bindings.
25. [`VulkanDepth.cppm`](../VulkanDepth.cppm), [`VulkanShadowMap.cppm`](../VulkanShadowMap.cppm) and [`VulkanViewportTarget.cppm`](../VulkanViewportTarget.cppm): offscreen and depth attachment ownership.
26. [`VulkanBackendContext.cppm`](../VulkanBackendContext.cppm), [`VulkanTriangle.cppm`](../VulkanTriangle.cppm) and [`VulkanRenderer.cppm`](../VulkanRenderer.cppm): execution flow.
27. [`shaders/triangle.vert`](../shaders/triangle.vert), [`shaders/triangle.frag`](../shaders/triangle.frag), [`shaders/shadow.vert`](../shaders/shadow.vert) and [`shaders/debug_line.vert`](../shaders/debug_line.vert): CPU/GPU data-layout boundary.

**Checkpoint:** follow acquire -> record -> submit -> wait/reuse -> present
and explain what happens on swapchain recreation. Never equate a logical
render-graph transition with a Vulkan image-layout barrier without tracing
native command recording.

## 4. Compare other backends and validate platform gates

28. [`MetalRenderer.cppm`](../MetalRenderer.cppm) and [`detail/MetalBackend.mm`](../detail/MetalBackend.mm): cross-language Metal ownership and asynchronous completion.
29. [`OpenGLRenderer.cppm`](../OpenGLRenderer.cppm) and [`OpenGLShaders.cppm`](../OpenGLShaders.cppm): context-bound pipeline/state.
30. [`Direct3D12Renderer.cppm`](../Direct3D12Renderer.cppm) and [`detail/Direct3D12Backend.cpp`](../detail/Direct3D12Backend.cpp): Windows/DXGI/D3D12 command/resource lifetime.
31. [`examples/clear_window.cpp`](../examples/clear_window.cpp): simple runtime entry and observed backend.
32. [`tests/RendererTests.cpp`](../tests/RendererTests.cpp), [`tests/SkinningContractTests.cpp`](../tests/SkinningContractTests.cpp) and [`tests/PhysicsDebugBridgeTests.cpp`](../tests/PhysicsDebugBridgeTests.cpp): shared behavior.
33. [`tests/VulkanSkinningSmoke.cpp`](../tests/VulkanSkinningSmoke.cpp), [`tests/MetalSkinningSmoke.cpp`](../tests/MetalSkinningSmoke.cpp), [`tests/OpenGLSkinningSmoke.cpp`](../tests/OpenGLSkinningSmoke.cpp) and [`tests/Direct3D12SkinningSmoke.cpp`](../tests/Direct3D12SkinningSmoke.cpp): host-native smoke gates.
34. Read the remaining first-party shaders, helpers and automation in the complete source inventory below.

**Checkpoint:** document the exact OS, compiler, GPU/driver, backend,
image result and tests actually executed. Synthetic platform-policy
success is not a substitute for native backend validation.

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


## Complete source-file inventory

After the curated teaching path above, use this ordered checklist to reach every first-party source, script, shader and test file indexed in this branch. Group order follows concepts before operations, tooling and evidence; follow module imports when a different within-group order is more helpful. Vendored/dependency code, binaries, data and generated artifacts are excluded, not relicensed.

### Entry points and build (1)

1. [`CMakeLists.txt`](../CMakeLists.txt)

### Interfaces and modules (37)

1. [`AssetAdapters.cppm`](../AssetAdapters.cppm)
2. [`Camera.cppm`](../Camera.cppm)
3. [`DebugDraw.cppm`](../DebugDraw.cppm)
4. [`detail/Direct3D12Backend.hpp`](../detail/Direct3D12Backend.hpp)
5. [`detail/MetalBackend.hpp`](../detail/MetalBackend.hpp)
6. [`Direct3D12Renderer.cppm`](../Direct3D12Renderer.cppm)
7. [`GraphicsBackend.cppm`](../GraphicsBackend.cppm)
8. [`KairoRenderer.cppm`](../KairoRenderer.cppm)
9. [`Material.cppm`](../Material.cppm)
10. [`Mesh.cppm`](../Mesh.cppm)
11. [`MetalRenderer.cppm`](../MetalRenderer.cppm)
12. [`OpenGLRenderer.cppm`](../OpenGLRenderer.cppm)
13. [`OpenGLShaders.cppm`](../OpenGLShaders.cppm)
14. [`PhysicsDebugBridge.cppm`](../PhysicsDebugBridge.cppm)
15. [`Renderer.cppm`](../Renderer.cppm)
16. [`RendererTypes.cppm`](../RendererTypes.cppm)
17. [`RenderGraph.cppm`](../RenderGraph.cppm)
18. [`RenderScene.cppm`](../RenderScene.cppm)
19. [`ShadowSettings.cppm`](../ShadowSettings.cppm)
20. [`Skinning.cppm`](../Skinning.cppm)
21. [`Texture.cppm`](../Texture.cppm)
22. [`VulkanBackendContext.cppm`](../VulkanBackendContext.cppm)
23. [`VulkanBuffer.cppm`](../VulkanBuffer.cppm)
24. [`VulkanCommand.cppm`](../VulkanCommand.cppm)
25. [`VulkanDepth.cppm`](../VulkanDepth.cppm)
26. [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm)
27. [`VulkanDevice.cppm`](../VulkanDevice.cppm)
28. [`VulkanInstance.cppm`](../VulkanInstance.cppm)
29. [`VulkanRenderer.cppm`](../VulkanRenderer.cppm)
30. [`VulkanShadowMap.cppm`](../VulkanShadowMap.cppm)
31. [`VulkanSurface.cppm`](../VulkanSurface.cppm)
32. [`VulkanSwapchain.cppm`](../VulkanSwapchain.cppm)
33. [`VulkanSync.cppm`](../VulkanSync.cppm)
34. [`VulkanTexture.cppm`](../VulkanTexture.cppm)
35. [`VulkanTriangle.cppm`](../VulkanTriangle.cppm)
36. [`VulkanViewportTarget.cppm`](../VulkanViewportTarget.cppm)
37. [`Window.cppm`](../Window.cppm)

### Implementation and other source (3)

1. [`detail/Direct3D12Backend.cpp`](../detail/Direct3D12Backend.cpp)
2. [`detail/MetalBackend.mm`](../detail/MetalBackend.mm)
3. [`examples/clear_window.cpp`](../examples/clear_window.cpp)

### Shaders and GPU programs (7)

1. [`shaders/debug_line.frag`](../shaders/debug_line.frag)
2. [`shaders/debug_line.vert`](../shaders/debug_line.vert)
3. [`shaders/shadow.vert`](../shaders/shadow.vert)
4. [`shaders/skinned_shadow.vert`](../shaders/skinned_shadow.vert)
5. [`shaders/skinned_triangle.vert`](../shaders/skinned_triangle.vert)
6. [`shaders/triangle.frag`](../shaders/triangle.frag)
7. [`shaders/triangle.vert`](../shaders/triangle.vert)

### Automation and tooling (1)

1. [`.github/scripts/use_assets_integration_ref.py`](../.github/scripts/use_assets_integration_ref.py)

### Verification and tests (9)

1. [`tests/Direct3D12SkinningSmoke.cpp`](../tests/Direct3D12SkinningSmoke.cpp)
2. [`tests/MetalSkinningSmoke.cpp`](../tests/MetalSkinningSmoke.cpp)
3. [`tests/OpenGLSkinningSmoke.cpp`](../tests/OpenGLSkinningSmoke.cpp)
4. [`tests/PhysicsDebugBridgeTests.cpp`](../tests/PhysicsDebugBridgeTests.cpp)
5. [`tests/RendererTests.cpp`](../tests/RendererTests.cpp)
6. [`tests/RenderGraphExecutionPlanTests.cpp`](../tests/RenderGraphExecutionPlanTests.cpp)
7. [`tests/RenderGraphScaleTests.cpp`](../tests/RenderGraphScaleTests.cpp)
8. [`tests/SkinningContractTests.cpp`](../tests/SkinningContractTests.cpp)
9. [`tests/VulkanSkinningSmoke.cpp`](../tests/VulkanSkinningSmoke.cpp)

Index snapshot: 58 first-party source/build/test files among 73 tracked files. The curated list remains the conceptual starting point. This inventory alone does not mean those functions have already been annotated or tested.
