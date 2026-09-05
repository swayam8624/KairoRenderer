# Render Graph Execution Plan

This document records the backend-facing contract produced by `Kairo.Renderer.RenderGraph`.
It is deliberately narrower than a claim that native render-pass migration is complete.
The graph compiler owns ordering, hazards, logical state transitions, transient lifetimes,
and alias-slot planning. Vulkan, Metal, Direct3D 12, and OpenGL remain responsible for
translating that immutable plan into API-specific recording and resource objects.

## Compiled contract

A `CompiledRenderGraph` now exposes:

- every logical `RenderResourceDesc` by stable `RenderResourceHandle`;
- deterministic topological pass order;
- each pass's declared `RenderResourceAccess` records;
- exact logical `RenderResourceTransition` records before each pass;
- first/last-use lifetimes for every used resource;
- transient alias-slot assignments;
- physical capacity required by each alias slot; and
- the total transient allocation capacity after aliasing.

The transient byte total is the sum of physical alias-slot capacities, not the sum of every
logical transient resource. Non-overlapping compatible lifetimes may therefore share one
backend allocation without the backend needing to repeat graph lifetime analysis.

## Transition execution

`CompiledRenderGraph::Execute()` accepts an optional `RenderGraphExecutionHooks` value.
`ApplyTransitions` runs immediately before the corresponding pass callback and receives the
compiled transition list. Native backends can use this hook during staged migration to record
barriers before recording pass work.

Transition-hook time is intentionally included in the CPU pass profile because barrier
recording is part of command recording. If transition translation throws, the pass callback
and every later pass are skipped and the original exception propagates.

## Resource safety rules

- Graph-owned transient resources must be textures or buffers and start in `Undefined` state.
- `External` resources are never transient because the graph does not own their allocation.
- A resource is considered initialized only when it has a non-`Undefined` initial state or a
  prior write in graph declaration order.
- `ReadWrite` requires an initialized value.
- A pass may reference a logical resource once; combined behavior uses `ReadWrite`.

These rules keep native backends from receiving a plan that reads undefined memory or tries to
allocate an externally owned swapchain/resource through transient aliasing.

## Native migration sequence

The execution-plan contract is the prerequisite for, not a substitute for, native migration.
The remaining renderer work should proceed in this order:

1. split the current backend-frame envelope into common logical shadow, opaque, transparent,
   debug, tooling, readback, and present passes;
2. map compiled transitions to Vulkan pipeline barriers and D3D12 resource barriers, with
   equivalent Metal encoder/resource synchronization policy and explicit OpenGL ordering;
3. allocate transient render targets from `AliasSlots()` rather than backend-local lifetime
   guesses;
4. move capture/picking/readback dependencies into graph resources and passes;
5. add native GPU timestamps around the same logical pass identities;
6. add upload budgets, frame-latency/resource-pressure telemetry, and deferred destruction;
7. only then expand shadow topology to cascades, cube shadows, and atlases.

A backend is considered migrated only when its native smoke tests prove that these logical
passes execute with equivalent visible output and readback/picking behavior.
