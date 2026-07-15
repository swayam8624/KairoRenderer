# KairoRenderer

`KairoRenderer` is Kairo's real-time Vulkan renderer. It is deliberately
separate from `KairoRayTracer`: the ray tracer produces offline CPU images,
while this repository owns an interactive window, GPU device path, viewport,
debug draw, and later editor rendering.

## Current milestone

M3 is complete: GLFW window ownership, Vulkan instance and surface creation,
device/queue selection, swapchain acquisition, synchronization, transfer
clearing, and presentation. `KairoRendererClear` presents a dark blue Vulkan
frame in a real native window.

## Build

```bash
cd /Users/swayamsingal/Desktop/Programming/Kairo/KairoRenderer
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build
ctest --test-dir build --output-on-failure
./build/KairoRendererClear
```

The sample creates a native Vulkan window and continuously presents a cleared
frame until the window is closed. Resizing is supported; minimizing pauses
submission until the framebuffer is restored.

## Dependencies

Resolution prefers local Kairo packages:

```text
KairoMath      ../Foundation/KairoMath -> GitHub fallback
KairoGeometry  ../Foundation/KairoGeometry -> GitHub fallback
```

On macOS, the project links GLFW and MoltenVK. The Homebrew Vulkan headers are
required for compilation:

```bash
/opt/homebrew/bin/brew install vulkan-headers vulkan-loader glfw molten-vk shaderc glslang
```

The renderer uses Vulkan API design with MoltenVK as the macOS implementation.
Physics remains independent: renderer examples may consume physics debug data,
but `KairoPhysicsEngine` never imports this repository.

## Roadmap

```text
M1 window + Vulkan instance + surface       complete
M2 physical device + queues                  complete
M3 swapchain + command buffers + clear       complete
M4 shader pipeline + triangle
M5 mesh buffers + camera + cube
M6 debug draw + KairoPhysicsEngine shapes
M7 forward mesh rendering + lighting
M8 PBR materials + shadows
```
