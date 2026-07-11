# Frame graph API

Header: `engine/renderer/FrameGraph.hpp`.

`FrameGraph` is the backend-neutral execution, hazard-validation, logical-lifetime, and transient-allocation planner. It compiles declared pass/resource intent into a deterministic execution order, barrier intents, resource activation/retirement points, and alias-compatible allocation slots. A backend supplies callbacks that realize those contracts; Vulkan handles and VMA allocations do not leak into this API.

Backend integration points:

- `engine/renderer/vulkan/VulkanRenderer.FrameResources.cpp` — constructs and compiles the cached graph variants and records compile/allocation diagnostics.
- `engine/renderer/vulkan/VulkanRenderer.Frame.cpp` — executes the selected graph, translates barrier intents, dispatches dynamic-rendering passes, and binds logical resources to Vulkan objects.
- `engine/renderer/vulkan/VulkanRenderer.Sync.cpp` — maps graph access/usage states to Vulkan image and buffer synchronization.
- `engine/renderer/vulkan/VulkanRenderer.Swapchain.cpp` — realizes graph-owned depth/HDR resources transactionally and recreates them with the swapchain.

## Scale and handles

- Pass/resource handles use wide opaque indices with `kInvalidIndex = 0xffffffff`.
- Pass, resource, and edge records are vector-backed; there is no fixed small graph cap.
- Handles remain stable because records are appended and never erased.
- Adding records throws only if the handle index range is exhausted or allocation fails.

Compilation indexes edges by pass and resource, so dependency construction scales with declared edges rather than rescanning the full edge list for every pass/resource pair.

## Enums

`FrameGraphResourceKind`:

- `Image`
- `Buffer`

`FrameGraphAccess`:

- `Read`
- `Write`

`FrameGraphUsage`:

- `ColorAttachment`
- `DepthAttachment`
- `SampledImage`
- `TransferSource`
- `Present`
- `UniformBuffer`
- `StorageBuffer`
- `IndirectBuffer`
- `TransferDestination`
- `HostRead`

## Handles

- `ResourceHandle`
- `PassHandle`

Each stores an internal index and has `valid()`. Passing an invalid or out-of-range handle throws.

## Descriptors

`ResourceDesc`:

- `name`
- `kind`
- `imported`
- `transientBytes`
- `transientAlignment`
- `aliasClass`
- `hasFinalUsage`
- `finalUsage`

`PassDesc`:

- `name`
- `debugColor`

`Edge`:

- `pass`
- `resource`
- `access`
- `usage`
- `attachment`
- `load`
- `store`

## Building a graph

```cpp
ve::FrameGraph graph;
auto depth = graph.addResource({
    .name = "Depth",
    .kind = ve::FrameGraphResourceKind::Image,
    .transientBytes = depthAllocationBytes,
    .transientAlignment = depthAlignment,
    .aliasClass = depthAliasClass});
auto scene = graph.addPass({.name = "HDR Scene"});
graph.writeAttachment(
    scene, depth, ve::FrameGraphUsage::DepthAttachment,
    ve::FrameGraphAttachmentLoad::Clear,
    ve::FrameGraphAttachmentStore::Discard);
graph.compile();
```

Operations:

- `addResource(ResourceDesc) -> ResourceHandle`
- `addPass(PassDesc) -> PassHandle`
- `read(pass, resource, usage)`
- `write(pass, resource, usage)`
- `readAttachment(pass, resource, usage, load, store)`
- `writeAttachment(pass, resource, usage, load, store)`
- `setFinalUsage(resource, usage)`

- Adding the same `(pass, resource, access, usage)` edge twice throws immediately. A pass may declare at most one access state for a given resource; a second edge with different access or usage also throws. Split the work into separate passes to model ordering and synchronization.

## Validation on `compile()`

`compile()` checks:

- every pass has at least one resource edge.
- non-imported resources are written before any pass reads them.
- every non-imported resource has a nonzero byte size and power-of-two alignment.
- image/buffer usages match the resource kind and read-only/write-only usage rules.
Compilation derives RAW, WAR, and WAW hazard dependencies and exposes a stable topological pass order. The declaration index breaks ties, so independent passes remain deterministic.
For every used resource, compilation records first/last use, activation/retirement points, and barrier intents. Non-imported resources receive deterministic transient slots; non-overlapping resources alias only when their kind and nonzero alias class match. `TransientStats` reports logical requested bytes, physical slot bytes, aliased bytes, and slot count.
The graph owns these logical contracts, not native memory. Backend lifecycle callbacks bind or realize physical resources transactionally and must retire active resources on both success and failure.

## Querying

- `hasEdge(pass, resource, access, usage)`
- `hasFinalUsage(resource)`
- `finalUsage(resource)` — throws if none is set.
- `pass(handle)`
- `resource(handle)`
- `passCount()`
- `resourceCount()`
- `edgeCount()`
- `compiled()`
- `executionOrder() -> const std::vector<PassHandle>&` — compiled stable pass order; empty while invalidated.
- `lifetime(resource) -> const ResourceLifetime&` — first/last compiled pass and `used`; throws while the graph is invalidated.
- `barrierPlan() -> const std::vector<BarrierIntent>&` — available after compilation and may be empty; pass-associated intents follow execution order and final transitions follow them. It is cleared when the graph is invalidated.
- `finalBarrierIntent(resource) -> const BarrierIntent&` — O(1) lookup of the unique final transition; throws if the graph is uncompiled or no final transition exists.
- `barrierIntent(pass, resource, access, usage) -> const BarrierIntent&` — exact pass/resource state-change intent; throws if the selected tuple is absent or ambiguous.
- `transientAllocation(resource) -> const TransientAllocation&` — physical slot, slot capacity/alignment, and whether that slot aliases multiple logical resources.
- `transientStats() -> const TransientStats&` — requested, allocated, and aliased bytes plus physical slot count.

## Execution

`execute(callbacks, state)` walks the compiled plan without allocating in the steady state:

1. activate non-imported resources at first use;
2. apply the pass's ordered barrier intents;
3. invoke the pass with a `PassResources` view containing only its declared edges;
4. retire resources after last use, or after a required final transition.

`ExecutionResult` identifies create, transition, pass, or retirement failure and reports completed work. On failure, active resources are unwound in reverse creation order. `ExecutionState` is caller-owned reusable storage; imported resources never invoke lifecycle callbacks.

## Current renderer use

The Vulkan backend caches separate graph topologies at startup:

- depth-prepass-on versus depth-prepass-off are constructed separately, so the no-prepass variant omits the HDR depth-read edge instead of filtering only the prepass node.
- screenshot-enabled versus screenshot-disabled are independent variant bits.
- `Auto` caches four combinations; `ForceOn` and `ForceOff` cache only the valid depth combinations.

Each frame selects the matching cached topology and calls `FrameGraph::execute`. When GPU-generated submission is active, callbacks record temporal visibility cull/command generation, optional depth prepass, HDR scene, current-depth pyramid build, tonemap/ImGui, and optional screenshot copy; no parallel manual pass path remains. Transition callbacks consume the graph barrier plan and tracked live Vulkan state to emit image/buffer barriers, including final present and host-read transitions.

Depth and HDR are graph-owned logical transients realized transactionally at swapchain scope. GPU cull buffers and the half-resolution depth pyramid are imported backend allocations: cull reads the previous completed pyramid before current rendering, and the later pyramid pass replaces it for the next same-queue submission. Imported read-before-write ordering is covered by the graph's WAR plan. Per-pass GPU timestamps distinguish cull, depth, HDR, pyramid, and final work; graph structure/barrier/allocation/recompile diagnostics are exposed through `RenderStats`, ImGui, and the machine-readable run summary.

Swapchain images and screenshot readback buffers are imported. Acquired swapchain state explicitly chains the image-available semaphore wait into the first graph transition; read-only attachments use `VK_ATTACHMENT_STORE_OP_NONE` so discard policy does not introduce a hidden write. Post-acquire failures restore tracked state and recreate the swapchain before semaphore reuse.
