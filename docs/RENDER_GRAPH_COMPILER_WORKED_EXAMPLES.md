# Render graph compiler: hazards, topological sorting and transient aliasing

This tutorial is an algorithmic deep dive into [RenderGraph.cppm](../RenderGraph.cppm). Follow the actual public APIs **AddResource**, **AddPass**, **DependsOn**, **Compile** and **CompiledRenderGraph::Execute**. They form a CPU scheduling/description system; they do not automatically allocate a native image or insert Vulkan/Metal resource barriers.

For native ownership, also read [Renderer.cppm](../Renderer.cppm) and the broader [frame-lifecycle walkthrough](FRAME_LIFECYCLE_WALKTHROUGH.md).

## 1. Two graphs, not one

The **authoring graph** stores logical resource declarations, pass accesses and explicit dependencies. The **compiled graph** stores a topologically sorted pass schedule with logical state transitions, resource lifetime intervals, alias-slot estimates and callbacks.

A RenderResourceHandle is an index in this graph's resource array. A RenderPassHandle is a different index in the pass array. Neither is a VkImage/MTLTexture/D3D12 descriptor handle, and the numbers must not be interpreted as native allocation identities.

## 2. The meaning of each access mode

| Mode | Meaning | Required ordering |
| --- | --- | --- |
| Read | Existing initialized content is consumed | After prior writer |
| Write | Prior content is replaced | After prior writer and pending readers |
| ReadWrite | Prior content is read and then modified | After initialization, prior writer and pending readers |

One pass may name a given resource only once; ReadWrite represents an in-pass read+write. Undefined is an initial-resource state, **not** a valid requested pass state.

An external/nontransient resource can have a defined initial state. A transient resource must begin Undefined. Reading that resource before a prior write is rejected, even when a different pass might write it later.

## 3. A three-pass example

Declare an HDR color target initially Undefined, and suppose passes are registered in this order:

1. **GBuffer** writes the HDR target as ColorAttachment.
2. **Tonemap** reads HDR target as ShaderRead.
3. **Blur** requests ReadWrite on HDR as General.

The hazard state initially has no last writer, no readers and Initialized=false. GBuffer marks it initialized and becomes the last writer. Tonemap acquires an edge from GBuffer and enters the pending readers list. Blur acquires an edge from GBuffer and *all pending readers* (including Tonemap), then clears the readers list and becomes the new writer.

The resulting dependency requirements are GBuffer → Tonemap, GBuffer → Blur and Tonemap → Blur. The second edge may be redundant for reachability but the compiler deduplicates identical edges before increasing indegree.

**Exercise:** make Tonemap the first registered pass while keeping an explicit dependency on a later-registered GBuffer. **Compile still rejects the read-before-first-write** because its hazard discovery processes passes in declaration order. Explicit topological dependencies do not make a later declaration count as earlier initialization in the current implementation.

## 4. Why explicit and implicit edges must be combined

**DependsOn(pass, dependency)** adds the directed edge dependency → pass. It immediately rejects self-dependencies and duplicates in that pass's explicit list. The compiler also creates hazard edges. When both mechanisms add the same edge, the adjacency set ensures the indegree is incremented only once.

The compiler runs Kahn's topological sort, using a min-index priority queue for ready nodes. That yields deterministic ordering for independent ready passes with equal dependencies. If the resulting order contains fewer passes than declared, there is a cycle and compilation throws.

Deterministic pass order does not imply deterministic GPU execution time or that the native driver submits the same graphics commands. This only fixes the CPU plan.

**Exercise:** create independent passes A and B and a final C depending on both. Verify the stable A/B selection. Add a reverse C→A dependency and ensure a cycle is rejected instead of hanging.

## 5. How logical resource transitions are computed

For each resource, the compiler starts with its declared InitialState. It walks *compiled pass order*, compares the last tracked state with the next access's requested state, and emits a transition record when they differ.

For an HDR resource, a possible state sequence is:

- Undefined → ColorAttachment for the first write;
- ColorAttachment → ShaderRead for a later sampling pass;
- ShaderRead → General for a subsequent ReadWrite compute-style pass.

Those transition records specify **intent**. Actual Vulkan layouts and pipeline-stage/access masks, Metal hazards, D3D12 resource barriers, queue ownership and readback waits must be implemented by the selected native backend. A generated transition list is not a validation that those native barriers were executed.

**Execute** loops compiled passes, invokes an optional transition-recording hook before the pass callback, and measures elapsed CPU time. A callback exception propagates and stops later callbacks; already-recorded or submitted GPU work is not automatically rolled back.

## 6. Why transient lifetimes can share memory

A resource lifetime is the inclusive interval from its first compiled pass access to its last access. A transient texture used by passes 0–2 cannot share physical capacity with another resource that begins at pass 2, because their lifetimes overlap at that pass. Sharing is allowed only when the earlier **LastPass < later FirstPass**.

The implementation groups by RenderResourceKind and requires an existing slot's Capacity to be at least the new resource's EstimatedBytes. It uses a deterministic greedy first-fit selection. A new slot is allocated in the *logical plan* when none qualifies.

| Resource | Kind | Estimated size | Lifetime |
| --- | --- | ---: | --- |
| A | Texture | 8 MiB | passes 0–2 |
| B | Texture | 6 MiB | passes 3–4 |
| C | Buffer | 4 MiB | passes 3–5 |
| D | Texture | 12 MiB | passes 6–7 |

A and B can reuse the same 8 MiB texture slot because their lifetimes do not overlap and A's capacity is sufficient for B. C needs a distinct buffer slot even when time intervals do not overlap, because resource kinds differ. D cannot reuse the 8 MiB slot and needs another 12 MiB texture slot. The logical transient allocation estimate in this scenario is **24 MiB** (8+4+12), not the sum of resource bytes (30 MiB).

These values are an example of the algorithm, not measured device memory. Native alignment, upload/staging, heap reservations, alias barriers and driver overhead are not modeled by this estimate. The greedy first-fit plan is not a guaranteed globally minimal allocator.

## 7. Two error-handling details worth testing

The current **AddResource** code inserts a resource Name into its uniqueness set before checking all later constraints and before appending the resource. If a subsequent validation or allocation throws, the Name can remain reserved without any corresponding resource. **AddPass** has the analogous behavior.

These are source-observed potential exception-safety gaps. The teaching-only changes intentionally do not alter this runtime behavior. Regression tests should attempt an invalid first registration followed by a corrected declaration with the *same* Name, and confirm the registry is not poisoned. A separate functional change can reorder validation or implement rollback.

Another important boundary: the graph uses declaration order for hazard discovery and compiled order for transition/lifetime planning. Both must be reflected in tests, or refactoring pass registration may accidentally change semantics.

## 8. Test-driven study sequence

Read [RenderGraphExecutionPlanTests.cpp](../tests/RenderGraphExecutionPlanTests.cpp), [RenderGraphScaleTests.cpp](../tests/RenderGraphScaleTests.cpp) and [RendererTests.cpp](../tests/RendererTests.cpp).

For a new test, construct a graph with one uninitialized read and confirm Compile rejects it; another with a write-after-read hazard and confirm the expected topological ordering; a cycle with an explicit dependency; lifetimes with boundary overlap (LastPass equal FirstPass must not alias); and resource kinds/capacity mismatches. Confirm also that Execute's transition hook fires before each callback and that CPU profile names correspond to compiled order.

Then use a real Vulkan/Metal/D3D12/OpenGL backend smoke test for native barriers and image output. Successful logical graph tests cannot substitute for this host-specific verification.

*Status: source-derived lesson. No new C++ or GPU build was run as part of this documentation-only pass.*
