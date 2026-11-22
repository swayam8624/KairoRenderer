# Native GPU resource ownership, frame synchronization and readback — Vulkan vs. Metal

This lesson continues [the KairoRenderer source reading order](SOURCE_READING_ORDER.md), [the authored-mesh-to-frame walkthrough](FRAME_LIFECYCLE_WALKTHROUGH.md), and [the render graph compiler exercises](RENDER_GRAPH_COMPILER_WORKED_EXAMPLES.md). It follows actual source, not an idealized engine design:

- [`VulkanRenderer.cppm`](../VulkanRenderer.cppm), [`VulkanSync.cppm`](../VulkanSync.cppm), [`VulkanCommand.cppm`](../VulkanCommand.cppm), [`VulkanSwapchain.cppm`](../VulkanSwapchain.cppm), [`VulkanBuffer.cppm`](../VulkanBuffer.cppm)
- [`MetalRenderer.cppm`](../MetalRenderer.cppm) and the native [`detail/MetalBackend.mm`](../detail/MetalBackend.mm)
- [`RenderGraph.cppm`](../RenderGraph.cppm), plus [unit tests](../tests/RenderGraphExecutionPlanTests.cpp), [Vulkan skinning smoke](../tests/VulkanSkinningSmoke.cpp), and [Metal skinning smoke](../tests/MetalSkinningSmoke.cpp)

**Evidence status:** source-level inspection, comments and test-to-implementation mapping. No Vulkan/Metal native execution, measured GPU timing, validation-layer output or sanitizer evidence was produced during this documentation pass.

## 1. One resource has several different identities

An authored KairoAssets mesh can be referred to by a durable asset identifier; a scene submission refers to a runtime `MeshHandle`; the backend maps that handle to one or more native allocations (for Vulkan, buffers and memory; for Metal, `id<MTLBuffer>` objects).

A render-graph `RenderResourceHandle` is a **logical scheduling ID**. An alias-slot ID describes possible reuse of a transient allocation. Neither is a Vulkan `VkBuffer`, `VkImage` or Metal `id<MTLTexture>`, and neither acquires GPU ownership by itself.

| Identity or object | Owner | Lifetime question |
| --- | --- | --- |
| AssetID and portable mesh/texture artifact | KairoAssets or project storage | Can the asset be located or re-imported? |
| MeshHandle / TextureHandle | Renderer backend API | Has this upload been created and not destroyed? |
| RenderResourceHandle and alias slot | one compiled `RenderGraph` | Does the logical pass schedule permit reuse without overlap? |
| VkBuffer / VkDeviceMemory / VkImageView | Vulkan resource wrapper or swapchain | Has all GPU work referencing this resource retired? |
| MTLBuffer / MTLTexture / CAMetalLayer | native Metal backend | Has the command buffer completed? Can resize replace the target? |
| Pick-result object ID | current frame's render submission | How does this draw ID map back to a stable authored entity? |

**Key invariant:** resource lifetime must be long enough for the *actual native GPU submission*, not merely for the C++ function or render-graph callback that encoded it.

## 2. A render graph dependency is not a Vulkan barrier

The CPU [`RenderGraph`](../RenderGraph.cppm) tracks each resource's logical state, read/write accesses and pass dependencies. It detects read-before-write and conflicting access, orders passes topologically, computes transitions and proposes alias slots when logical lifetimes do not overlap.

A compiled graph executes host callbacks in order and can invoke transition hooks. That helps with planning, but there is no automatic Vulkan `vkCmdPipelineBarrier` or Metal resource barrier merely because `RenderResourceState::ShaderRead` was recorded in a logical descriptor.

Native backend code must implement the relevant transitions, layout changes, pipeline synchronization and command recording. A CPU graph test passing does **not** prove safe GPU access, nor does a transient alias-slot estimate prove the GPU memory was actually shared.

This distinction is particularly visible in `VulkanRendererRuntime::DrawFrame`, which uses graph callbacks to impose host-level order while native semaphores and fences control GPU execution. In Metal, the graph controls when `MetalBackend::Draw` and optional `Pick`/`Capture` are called, but those native functions still enforce their own GPU completion.

## 3. The one-frame Vulkan state machine

The current [`VulkanRenderer.cppm`](../VulkanRenderer.cppm) owns one primary command buffer and a frame synchronization helper. Its DrawFrame graph describes:

```text
CPU RenderGraph:
    Retire -> Acquire -> Record -> Submit -> Present

Retire:
    vkWaitForFences(InFlight)             [the prior queue submission finished]
    CompleteViewportPick / Capture       [host-coherent readback now safe]

Acquire:
    vkAcquireNextImageKHR(ImageAvailable) [acquired image index i]
    OUT_OF_DATE? recreate and skip frame
    vkResetFences(InFlight)               [prepare this frame's signal]

Record:
    VulkanTriangle.Record(command,i,...) [real native draw/copy recording]

Submit:
    vkQueueSubmit(
        wait = ImageAvailable,
        stage = COLOR_ATTACHMENT_OUTPUT,
        command = command,
        signal = RenderFinished[i],
        fence = InFlight)

Present:
    vkQueuePresentKHR(
        wait = RenderFinished[i],
        swapchainImage = i)
    OUT_OF_DATE/SUBOPTIMAL? recreate
```

The fence starts **signaled** so the first Retire pass does not deadlock waiting on work that never existed. Once a frame is acquired, it is reset, and `vkQueueSubmit` is expected to signal it when the GPU reaches submission completion. The next Retire waits on that fence before reusing the command buffer and reading the GPU-produced data.

`ImageAvailable` signals that an image is available for rendering. `RenderFinished[i]` signals that queue work producing image `i` has finished far enough for presentation to wait. Those semaphores describe GPU/queue dependencies. A CPU fence wait additionally permits safe **host-side** reuse.

The current scheme is intentionally conservative: one reusable command buffer/fence is easier to reason about than a multi-frame ring, but limits CPU–GPU overlap. Changing it requires per-flight-frame command pools, fence tracking and per-swapchain-image resource retirement, not just increasing an array length.

## 4. The subtle difference between recording, submitting, and completing

`VulkanCommandBuffer::Begin` calls `vkResetCommandBuffer` then `vkBeginCommandBuffer`. `End` calls `vkEndCommandBuffer`. These are **CPU-side recording operations**; the GPU has not executed the commands merely because recording ended.

`vkQueueSubmit` transfers recorded work to the graphics queue; a successful return still does not mean that work has completed. That is why `CompleteViewportPick` and `CompleteViewportCapture` are in the following Retire stage **after the fence wait**.

`VulkanHostBuffer` is created as a persistently mapped `HOST_VISIBLE | HOST_COHERENT` allocation. CPU `Write` copies bytes into mapped memory; `Read` copies GPU-written bytes out. Coherent memory helps with host/device visibility requirements, but it does not synchronize producer/consumer execution. You must still wait for the relevant GPU write to complete before CPU readback, and must not mutate bytes concurrently with GPU reads.

The exposed stage-profile values come from the graph's CPU callback timings. They may include host-side waiting and submission time; they are **not GPU timestamp queries** or evidence of how much time each shader spent executing.

## 5. Swapchain images are borrowed; image views are owned

[`VulkanSwapchain`](../VulkanSwapchain.cppm) negotiates the surface extent, format, presentation mode and image count. It prefers `B8G8R8A8_SRGB` with SRGB nonlinear color space when supported, and MAILBOX presentation when available, otherwise FIFO.

It creates a `VkSwapchainKHR`, obtains its image handles via `vkGetSwapchainImagesKHR`, and creates an owned image view for each returned image. **The swapchain owns those VkImages.** Calling `vkDestroyImage` on them independently would be invalid; the wrapper destroys its image views, then the swapchain.

On resize/out-of-date presentation, `VulkanRendererRuntime::RecreateSwapchain` waits for the device to become idle, releases VulkanTriangle's swapchain-specific render targets, destroys/rebuilds the swapchain, recreates per-image sync objects, and finally reconstructs dependent render-passes/targets. The swapchain wrapper also contains an idle wait in its own Recreate. These waits make teardown simpler at the cost of resize latency.

A zero-size framebuffer cannot support a valid presentable extent and is handled specially: `DrawFrame` skips rendering when minimized, and swapchain creation refuses zero extent.

When graphics and presentation queue families differ, the swapchain selects concurrent sharing with both families. When they match, it uses exclusive sharing. This is a *queue-ownership configuration*, not a replacement for command or presentation synchronization.

## 6. Metal uses a command-buffer completion boundary

The native [`MetalBackend.mm`](../detail/MetalBackend.mm) obtains `MTLDevice`, a command queue and a `CAMetalLayer` attached to the GLFW Cocoa window. It owns pipeline states, offscreen color (`RGBA16Float`), ID (`R32Uint`), depth textures, a shadow map and native mesh/texture maps.

`MetalBackend::Draw(frame)` obtains an `MTLCommandBuffer`, encodes shadow and scene passes, encodes the final viewport-to-drawable/presentation pass when a drawable is available, commits the command buffer and stores it as `LastCommand`.

**A committed Metal command buffer is asynchronous.** The backend's `WaitForLastCommand()` calls `waitUntilCompleted`, checks for `MTLCommandBufferStatusError`, then clears the stored last-command reference. It is used before mesh/texture destruction, offscreen target replacement, picking and capture. The backend destructor also waits for outstanding last-command completion before native resources disappear.

`MetalRendererRuntime::DrawFrame` constructs `MetalFrame` on the host (including per-draw skin matrix arrays, lights and camera uniforms) and uses a short-lived CPU graph:

```text
RenderPresent [MetalBackend::Draw commits GPU command]
      |
      +-- PickReadback?   [MetalBackend::Pick awaits LastCommand, reads R32Uint]
      |
      +-- CaptureReadback? [MetalBackend::Capture awaits LastCommand, reads RGBA16F]
```

A pick requests **one object-ID pixel**, not a CPU ray cast through physics colliders. A capture converts Float16 color to 8-bit display RGBA, which loses HDR precision and is not an exact image archive. The order of optional pick and capture is reflected by graph dependencies, but the actual safety is established by the native completion wait.

The `ViewportTexture()` native pointer is bridged out as a **non-owning handle**. After `Resize` or backend destruction, the previous offscreen texture may no longer be valid. A UI must refresh its reference or establish explicit ownership if it intends to retain an object longer.

## 7. Why resource upload and teardown are different on the two APIs

**Vulkan host buffers:** `VulkanHostBuffer` explicitly creates `VkBuffer`, queries requirements, picks a memory type, allocates and binds `VkDeviceMemory`, and maps it. Destruction unmaps, frees memory and destroys the buffer. It borrows the device and cannot outlive it. No automatic resource fence exists inside this buffer wrapper.

**Metal meshes and textures:** `CreateMesh` stores native buffers in a map keyed by a generated renderer handle. `CreateTexture` allocates a shader-readable MTLTexture and its sampler; it writes exact mip byte ranges through `replaceRegion`. The material draw path resolves handles to map entries and uses fallback white/normal/black textures for absent optional channels. `DestroyMesh` and `DestroyTexture` wait for the last command before erasing a referenced entry.

Metal's shared-resource behavior and the retention of encoded objects are not permission to drop application-level lifetime tracking. Resource users can still be invalidated by an explicit map erase, target resize or backend shutdown.

## 8. Important error-path audits discovered in source

**Later repair status:** the command-pool, host-buffer, frame-sync, swapchain and depth-attachment partial-construction leak paths now have explicit cleanup logic. See [the completed Vulkan constructor-rollback source pass](VULKAN_PARTIAL_CONSTRUCTION_ROLLBACK_LAB.md). The table preserves the original **failure scenarios that still require native fault-injection tests**; the last row (frame fence reset before failed submission) is **not repaired**. These were source-audit findings, not observed driver crashes.

| Location | Failure path to test | Why it matters |
| --- | --- | --- |
| [`VulkanCommand.cppm`](../VulkanCommand.cppm) | `vkAllocateCommandBuffers` fails after successful `vkCreateCommandPool` | C++ does not execute the enclosing object's destructor if its constructor throws; without a local resource guard the pool can leak |
| [`VulkanBuffer.cppm`](../VulkanBuffer.cppm) | `vkAllocateMemory`, `vkBindBufferMemory` or `vkMapMemory` fails mid-construction | already-created VkBuffer/VkDeviceMemory needs explicit local cleanup on failure |
| [`VulkanSync.cppm`](../VulkanSync.cppm) | initial semaphore succeeds, fence creation fails | early failure is outside the per-image `try/catch` cleanup scope; successfully created handles can be leaked |
| [`VulkanSwapchain.cppm`](../VulkanSwapchain.cppm) | swapchain creation succeeds and later image-view creation fails | partial native state needs cleanup even though the enclosing constructor did not finish |
| [`VulkanRenderer.cppm`](../VulkanRenderer.cppm) | fence resets after acquire, then recording or submit throws before a successful queue submission | no submission may signal the reset fence; the next `vkWaitForFences(...,UINT64_MAX)` can stall indefinitely unless frame-state recovery recreates/signals the fence |

The local constructor-cleanup changes have now been committed, but require targeted injected-failure tests and Vulkan validation-layer evidence before they are certified. Merely catching a Vulkan error without deciding which native resources may still be in flight can make cleanup worse. Fence-reset recovery and queue/device-loss teardown remain separate tasks.

On Metal, verify that asynchronous commit errors are surfaced in every intended error path, and test resizing/destroying while a prior frame is in flight. A successful normal draw does not establish that every failure/teardown path is correct.

## 9. What the existing tests prove and do not prove

[`RendererTests.cpp`](../tests/RendererTests.cpp), [`RenderGraphExecutionPlanTests.cpp`](../tests/RenderGraphExecutionPlanTests.cpp), and [`RenderGraphScaleTests.cpp`](../tests/RenderGraphScaleTests.cpp) test render-graph hazard rules, transition-hook ordering, planned transient alias bytes, backend-policy selection, capture validation and a 256-resource graph planning case.

The [Vulkan smoke executable](../tests/VulkanSkinningSmoke.cpp) requests a Vulkan backend, draws a skinned geometry fixture, captures a frame **after the following fence-retirement frame**, then changes its joint palette and verifies rendered pixels differ. The [Metal smoke executable](../tests/MetalSkinningSmoke.cpp) performs analogous work on a native macOS Metal backend, with capture waiting for the command buffer within the capture path.

These programs are source-visible. They do not automatically mean that the correct native target was built or run on this particular revision, hardware or operating system. A skipped/unavailable presentation path is not a positive GPU test.

## 10. Host-qualified test and failure-check plan

On a compatible machine with GLFW, Vulkan headers/loader, a C++23 modules compiler and the repository's other requirements, the source defines `KairoRendererTests` and platform-dependent smoke targets in [`CMakeLists.txt`](../CMakeLists.txt). Suggested commands:

```bash
cmake -S . -B build -G Ninja \
  -DKAIRO_RENDERER_BUILD_TESTS=ON \
  -DKAIRO_RENDERER_ENABLE_VALIDATION=ON
cmake --build build --parallel 4
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
```

For *real GPU* validation, record the selected backend, exact commit SHA, OS, compiler, loader/driver, GPU, validation messages and actual test names/results. Run Metal smoke on macOS with a usable Metal device, Vulkan smoke with a Vulkan present-capable surface/device, and D3D12 smoke on Windows when supported. Neither cross-platform policy tests nor a CPU-only render graph compile substitute for those native gates.

To validate failure paths, inject allocation errors at distinct stages, force an out-of-date swapchain, minimize/restores windows, destroy/resize during pending GPU work, and deliberately fail a recording callback after fence reset. Assert cleanup and recovery rather than waiting on a fence that cannot be signaled. Run leak detectors and native GPU validation where practical.

**Next implementation improvement:** scoped native handle guards for partially constructed Vulkan resources; explicit frame state/error recovery for reset fences; per-flight-frame buffers and fences; timestamp queries to distinguish CPU waiting from GPU execution; accurate/efficient transient resource allocation behind the compiled render graph. These would be new runtime changes, not merely documentation.
