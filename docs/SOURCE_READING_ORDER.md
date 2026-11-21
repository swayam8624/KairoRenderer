# KairoRenderer — guided source reading order

Read this repository as a set of **shared rendering contracts** and **backend implementations**. KairoGameEngine's [RuntimeRenderBridge](https://github.com/swayam8624/KairoGameEngine/blob/main/Runtime/KairoPlayer/RuntimeRenderBridge.cppm) and realtime scene adapter translate durable project assets into session-local renderer objects. Rendering is GPU work; the optional [KairoRayTracer](https://github.com/swayam8624/KairoRayTracer) is a separate offline renderer.

## 1. Establish the public API

1. [README](../README.md), [CMakeLists](../CMakeLists.txt) — build flags, supported native APIs, tool requirements and tests.
2. [KairoRenderer.cppm](../KairoRenderer.cppm) — public re-exports; inspect which modules are gated by platform/build options.
3. [RendererTypes.cppm](../RendererTypes.cppm) — types/handles and validation boundaries.
4. [GraphicsBackend.cppm](../GraphicsBackend.cppm) — automatic or explicit API selection, availability and host-platform policy.
5. [Camera.cppm](../Camera.cppm), [RenderScene.cppm](../RenderScene.cppm), [Mesh.cppm](../Mesh.cppm), [Material.cppm](../Material.cppm), [Texture.cppm](../Texture.cppm) — what the caller is asking the GPU to render.
6. [AssetAdapters.cppm](../AssetAdapters.cppm) — conversion from [KairoAssets](https://github.com/swayam8624/KairoAssets) artifacts to renderer-friendly descriptors. Persistent asset handles must never be confused with GPU resource handles.

## 2. Learn frame scheduling and the backend facade

7. [RenderGraph.cppm](../RenderGraph.cppm) — logical passes, resource dependencies, access modes, state transitions and scheduling. Distinguish graph correctness from a particular GPU API's command recording.
8. [Renderer.cppm](../Renderer.cppm) — front-facing runtime facade choosing compiled Vulkan, Metal, D3D12 or OpenGL implementation. Player and Editor should depend on this API rather than on one native backend.
9. [DebugDraw.cppm](../DebugDraw.cppm), [ShadowSettings.cppm](../ShadowSettings.cppm), [Skinning.cppm](../Skinning.cppm) — secondary render features and their shared data contracts.

## 3. Follow one selected GPU implementation

10. [VulkanRenderer.cppm](../VulkanRenderer.cppm) → [VulkanInstance.cppm](../VulkanInstance.cppm), [VulkanDevice.cppm](../VulkanDevice.cppm), [VulkanSwapchain.cppm](../VulkanSwapchain.cppm), [VulkanCommand.cppm](../VulkanCommand.cppm), [VulkanSync.cppm](../VulkanSync.cppm), [VulkanDescriptor.cppm](../VulkanDescriptor.cppm) — explicit API device/surface/swapchain/commands and synchronization.
11. [MetalRenderer.cppm](../MetalRenderer.cppm) and [native backend detail](../detail/) — native macOS GPU resource ownership, command encoding and presentation.
12. [OpenGLRenderer.cppm](../OpenGLRenderer.cppm), [OpenGLShaders.cppm](../OpenGLShaders.cppm) — stateful graphics API and shader path.
13. [Direct3D12Renderer.cppm](../Direct3D12Renderer.cppm) — Windows-gated implementation. Do not infer macOS/Linux execution support from its presence in the source tree.

## 4. Validate integration and performance

14. [tests](../tests/) — graph ordering, backend policy, camera/scene/asset conversions and native smoke checks.
15. [KairoGameEngine player](https://github.com/swayam8624/KairoGameEngine/blob/main/Runtime/KairoPlayer/main.cpp) → [RealtimeSceneBridge](https://github.com/swayam8624/KairoGameEngine/blob/main/Runtime/KairoRealtimeRenderBridge/RealtimeSceneBridge.cppm) — actual integration path.
16. [STATUS.md](../STATUS.md) and [STATUS.yaml](../STATUS.yaml) — intended status must be reconciled with actual CI/build/test outputs.

## Cross-module tracing

**Project to GPU:** `Kairo.Assets` descriptors + scene instance identity → `AssetAdapters` / `RealtimeSceneBridge` → `RendererRuntime` creates local mesh/texture/material handles → chosen backend uploads resources → render graph schedules passes → swapchain/viewport target presents the image.

**Render graph:** high-level pass declares read/write access to logical resources → graph orders passes based on dependencies → backend records GPU transitions/commands. Logical dependencies are not automatically proof that every backend synchronization barrier is correct.

**Backend choice:** caller requests `GraphicsBackend::Automatic` or a specific API → platform/build availability checked in `GraphicsBackend` → `RendererRuntime` instantiates one compiled implementation. There is no universal GPU API silently available on all hosts.

## Exercises

1. Follow one imported texture from persistent asset ID to uploaded backend resource, then find where its GPU lifetime ends.
2. Draw a three-pass graph with a depth pass, lighting pass, and presentation; identify the resource hazards.
3. Compare how Vulkan and Metal create, submit and release command buffers.
4. Trace a render scene instance through its object ID used for picking.
5. Explain what tests are needed to establish parity between two render backends.

## Licensing

Original library source and educational documentation are intended for learning, redistribution and extension under [MIT LICENSE](../LICENSE). GLFW, Vulkan SDK, Metal, OpenGL and any bundled shader/third-party assets retain their own applicable notices and licensing terms.
