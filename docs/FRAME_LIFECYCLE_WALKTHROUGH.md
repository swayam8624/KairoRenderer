# From an authored mesh to a presented frame — KairoRenderer

This is a source-linked teaching companion to the
[complete reading order](SOURCE_READING_ORDER.md). It explains the *present*
backend-neutral and platform-native responsibilities. It is not a claim of
successful GPU execution or CI verification on every operating system.

## 1. Separate authored identity from runtime GPU ownership

Start in [KairoAssets](https://github.com/swayam8624/KairoAssets), where
`AssetRegistry` assigns persistent `AssetID` values and importer/artifact
code produces CPU-side mesh, material, texture and glTF scene records.

[`AssetAdapters.cppm`](../AssetAdapters.cppm) converts those records into
renderer-facing `Mesh`, `PBRMaterial` and texture-handle references. A
`MaterialTextureResolver` translates a typed texture asset reference to a
**runtime** `TextureHandle`. This is an explicit boundary:

* `AssetID`: stable authored reference, independent from the current file path
* `TextureArtifactData`: validated portable image/pixel+sampler description
* `TextureHandle`: opaque identity for a GPU resource owned by one renderer
* `MeshHandle`: analogous renderer-owned mesh resource identity

An absent *optional* texture is not the same as an authored texture reference
that cannot be resolved. The adapter must preserve the latter as an error,
rather than quietly change the material.

The backend that uploaded the resource must keep it alive while draw or
descriptor state refers to it. The project manifest is **not** a GPU-resource
lifetime manager.

## 2. Build the backend-neutral submission

[`RenderScene.cppm`](../RenderScene.cppm) stores CPU-side draw records,
lighting and environment data. Multiple `MeshDraw` instances can reuse the
same uploaded `MeshHandle` while retaining separate transforms, materials,
object IDs and optional skinning data.

[`RendererTypes.cppm`](../RendererTypes.cppm) and
[`Camera.cppm`](../Camera.cppm) define the shared viewport/camera contracts.
[`Skinning.cppm`](../Skinning.cppm) explains joint transforms and the
shader/CPU packing conventions. [`ShadowSettings.cppm`](../ShadowSettings.cppm)
defines tuning for the supported directional shadow path.

**Exercise:** find an Editor scene node, follow its mesh/texture IDs to
`MakeGltfRenderAsset`, and identify the point where stable source IDs become
renderer handles. Then follow that handle to its eventual backend destructor.

## 3. Decide *which graphics API* runs

[`GraphicsBackend.cppm`](../GraphicsBackend.cppm) distinguishes a platform
(`HostPlatform`) from compiled implementation availability
(`GraphicsBackendAvailability`). An automatic request selects a preferred
compiled compatible API according to policy; an explicit unavailable API is
not silently changed into another backend.

[`Renderer.cppm`](../Renderer.cppm) implements `RendererRuntime`, a
facade whose variant owns **one** selected native runtime and delegates
`SubmitRenderScene`, mesh/texture lifetime, camera, viewport operations,
picking, capture, overlays and `DrawFrame`.

A simulated backend-policy test can confirm selection rules without creating
any Metal/Vulkan/D3D12 device. Conversely, successful native Windows D3D12
execution requires a Windows machine/CI runner and valid driver environment.

## 4. What the logical render graph actually does

[`RenderGraph.cppm`](../RenderGraph.cppm) has two phases.

**Authoring:** `AddResource` declares named resources and initial states;
`AddPass` names a pass with access modes and required states;
`DependsOn` adds explicit ordering. Each pass must have at least one
resource access, and a resource cannot be accessed twice within the
same pass: use `ReadWrite` for that case.

**Compilation:** `Compile()` tracks the last writer, preceding readers
and initialization state for every logical resource. It rejects reads before
initialization, adds hazard/dependency edges and computes a topological order,
rejecting cycles. It records state-transition requests and each resource's
first/last pass in that order.

Transient resources with **strictly disjoint** lifetimes may reuse a logical
alias slot when their kind matches and the previous slot capacity is large
enough. These are **estimated allocation capacities**, not native Vulkan
memory bindings or actual Metal heaps.

`CompiledRenderGraph::Execute` calls an optional transition hook and a
pass callback in compiled order, returning CPU pass durations. Exceptions
stop subsequent callbacks; they do not undo GPU work previously recorded.

**Current integration boundary:** `RendererRuntime::DrawFrame` routes a
native backend frame through a graph envelope. Shadow, opaque, transparent,
debug, readback and present operations are **not yet guaranteed to be
individual common graph-scheduled native passes**. Do not infer physical
GPU memory aliasing or native pipeline barriers solely from a logical
`RenderResourceTransition`.

### A worked hazard example

```text
Pass A: write HDR target    (Undefined -> ColorAttachment)
Pass B: read HDR target     (ColorAttachment -> ShaderRead)
Pass C: write present image (Undefined -> ColorAttachment)
Pass D: present image       (ColorAttachment -> Present)
```

The graph must know that B depends on A. Whether C depends on B depends on
declared resource accesses and explicit edges. Real GPU barrier stages,
queue ownership and image-layout transitions still belong to the concrete
backend executor. Read
[`tests/RenderGraphExecutionPlanTests.cpp`](../tests/RenderGraphExecutionPlanTests.cpp)
before changing compiler hazard logic.

## 5. Follow Vulkan lifetime and synchronization

The explicit Vulkan study path is:

[`VulkanInstance.cppm`](../VulkanInstance.cppm) ->
[`VulkanSurface.cppm`](../VulkanSurface.cppm) ->
[`VulkanDevice.cppm`](../VulkanDevice.cppm) ->
[`VulkanSwapchain.cppm`](../VulkanSwapchain.cppm) ->
[`VulkanCommand.cppm`](../VulkanCommand.cppm) ->
[`VulkanSync.cppm`](../VulkanSync.cppm) ->
[`VulkanRenderer.cppm`](../VulkanRenderer.cppm).

A swapchain image is acquired before presentation; the GPU must finish
recorded work before its resources/command buffers are reused or destroyed.
`VulkanFrameSync` owns one acquire semaphore, image-index-dependent
completion semaphores and a fence for the current scheduling scheme.
Recreation requires appropriate device-idle synchronization before
destroying old image-dependent objects.

Study [`VulkanDescriptor.cppm`](../VulkanDescriptor.cppm),
[`VulkanBuffer.cppm`](../VulkanBuffer.cppm),
[`VulkanTexture.cppm`](../VulkanTexture.cppm) and
[`VulkanShadowMap.cppm`](../VulkanShadowMap.cppm) for native resource,
descriptor and image-layout responsibilities. CPU-side state descriptions
are not sufficient to prevent GPU data hazards.

## 6. Compare Metal, OpenGL and D3D12 without assuming parity

[`MetalRenderer.cppm`](../MetalRenderer.cppm) exposes a compatible scene
and viewport contract, with Objective-C/Metal implementation in
[`detail/MetalBackend.mm`](../detail/MetalBackend.mm).
Read the bridge together with draw, capture, picking, native completion and
resource destruction; asynchronous command buffers require explicit waits
before readback or release.

[`OpenGLRenderer.cppm`](../OpenGLRenderer.cppm) uses a different context/state
and synchronization model. [`Direct3D12Renderer.cppm`](../Direct3D12Renderer.cppm)
and [`detail/Direct3D12Backend.cpp`](../detail/Direct3D12Backend.cpp)
require Windows-native execution for definitive validation.

Passing a backend-neutral `RendererTests.cpp` on macOS does not assert
D3D12's real swapchain works. Compare the contracts and test lanes, not only
class or API method names.

## 7. What timing and memory counters mean

[`KairoRenderer.cppm`](../KairoRenderer.cppm) contains telemetry records.
`CpuGraphProfile` measures CPU callbacks/transition-hook recording time;
it does **not** measure GPU execution time. The mesh byte estimate sums CPU
vertex/index/skin buffer lengths, not backend padding, staging allocations,
descriptor heaps or GPU residency budgets. Scene draw count measures
submitted records, not pixels or visible meshes.

**Reproducible experiment:** for a fixed scene, record platform, renderer
selection, GPU, driver, compiler, resolution, per-backend features, scene
draw count and the exact native test. Run both resource-lifetime stress
checks and actual frame capture before claiming parity.

## 8. Tests, maintenance and licensing

Start with [`tests/RendererTests.cpp`](../tests/RendererTests.cpp),
[`tests/RenderGraphExecutionPlanTests.cpp`](../tests/RenderGraphExecutionPlanTests.cpp),
[`tests/RenderGraphScaleTests.cpp`](../tests/RenderGraphScaleTests.cpp) and
[`tests/SkinningContractTests.cpp`](../tests/SkinningContractTests.cpp).
Then inspect the appropriate backend-specific skinning smoke test on its
supported operating system.

Track whether each claim is **source-present**, **unit-test observed**,
**native-host verified** or **benchmark-evidenced**. KairoAssets source
licenses and imported images/models may have separate upstream terms;
a renderer's code license does not relicense an asset.

*Verification note: written from source inspection of the documentation
branch. No new native GPU test or benchmark is asserted here.*
