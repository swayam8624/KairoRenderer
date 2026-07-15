# KairoRenderer

`KairoRenderer` is Kairo's real-time Vulkan renderer. It is deliberately
separate from `KairoRayTracer`: the ray tracer produces offline CPU images,
while this repository owns an interactive window, GPU device path, viewport,
debug draw, and later editor rendering.

## Current milestone

M8 basic forward lighting is complete: GLFW window ownership, Vulkan instance and surface creation,
device/queue selection, swapchain presentation, synchronization, shader
compilation, uniform descriptors, a D32 depth attachment, a camera-driven
indexed mesh vertex/index buffer upload, per-vertex normals, a directional-light forward pass, and a dynamic world-space debug-line pipeline. `KairoRendererClear`
presents a rotating depth-tested cube with axes, an AABB, and a wire sphere in
a real native window.

## Build

```bash
cd /Users/swayamsingal/Desktop/Programming/Kairo/KairoRenderer
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build
ctest --test-dir build --output-on-failure
./build/KairoRendererClear
```

The sample creates a native Vulkan window and continuously presents a rotating
cube plus debug geometry until the window is closed. Resizing recreates
color/depth framebuffers; minimizing pauses submission until the framebuffer
is restored.

## Debug Draw Boundary

`DebugDrawList` is the renderer-neutral data contract used by physics, tools,
and game code. It owns no GPU state. Build a list each frame from world-space
positions, submit it, and reuse or clear the source list immediately:

```cpp
kairo::renderer::DebugDrawList debug;
debug.AddAABB(minimum, maximum, { 0.2f, 0.8f, 1.0f, 1.0f });
debug.AddContactNormal(point, normal);
runtime.SubmitDebugDraw(debug);
```

The renderer copies the submitted vertices, uploads them after the in-flight
frame fence completes, and draws thin depth-tested line segments in the same
camera space as the scene. This keeps `KairoPhysicsEngine` independent from
Vulkan while allowing an engine/editor adapter to translate its contact,
collider, and broadphase diagnostic data.

## Dependencies

Resolution prefers local Kairo packages:

```text
KairoMath      ../Foundation/KairoMath -> GitHub fallback
KairoGeometry  ../Foundation/KairoGeometry -> GitHub fallback
```

On macOS, the project links GLFW and the Vulkan loader; the loader discovers
MoltenVK as the installed Vulkan ICD. The Homebrew headers and loader are
required for compilation and GLFW surface discovery:

```bash
/opt/homebrew/bin/brew install vulkan-headers vulkan-loader glfw molten-vk shaderc glslang
```

When configured on macOS, `KairoRenderer` selects Homebrew's MoltenVK ICD only
if `VK_ICD_FILENAMES` is unset. An explicit environment value is preserved for
driver debugging or a custom Vulkan setup.

The renderer uses Vulkan API design with MoltenVK as the macOS implementation.
Physics remains independent: renderer examples may consume physics debug data,
but `KairoPhysicsEngine` never imports this repository.

## Roadmap

```text
M1 window + Vulkan instance + surface       complete
M2 physical device + queues                  complete
M3 swapchain + command buffers + clear       complete
M4 shader pipeline + triangle                complete
M5 camera uniform + depth-tested cube         complete
M6 GPU debug lines + external bridge contract   complete
M7 indexed mesh submission                    complete
M8 directional lighting                         complete
M9 material parameters + PBR foundation
M8 PBR materials + shadows
```
