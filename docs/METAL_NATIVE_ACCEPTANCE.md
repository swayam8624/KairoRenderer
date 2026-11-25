# Metal native acceptance

The host-neutral C++ tests do not compile or execute Metal shaders. Do not treat
a skipped CTest, cancelled macOS job, or Linux Vulkan success as Metal acceptance.

On a Mac with a working Metal device and the project's existing C++23/GLFW/Vulkan
build prerequisites, configure a standalone single-config Ninja build, enabling
`KAIRO_RENDERER_BUILD_TESTS`. Then execute:

```sh
bash scripts/require_metal_smoke.sh build
```

Use the actual configured build directory instead of `build` when different.
The script compiles both native smokes, enables the Metal debug layer and executes
both directly. Exit 77 (presentation unavailable) fails rather than skips.

Required observations:
- A 255-joint palette can animate the highest supported joint in a captured image.
- Instanced opaque meshes match separate mesh allocations and retain full 32-bit IDs.
- Switching back to ordinary draws preserves the reference image.
- A 256-line debug batch changes visible pixels without overwriting scene IDs.
- Clearing the debug list restores the baseline.

Retain the command logs in the build directory under `metal-smoke-diagnostics`.
Inspect Metal validation diagnostics; the script checks process exit status, not
all possible diagnostic messages. Record commit SHA, macOS version, compiler,
GPU device and result. Physical-GPU performance benchmarks are a separate gate.

Current limitations: shadow instancing and camera culling are not yet implemented
on Metal; instance, debug and skeletal frame buffers are independently allocated
rather than pooled across fence-retired frames.
