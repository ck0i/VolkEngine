# Frame graph API

Header: `engine/renderer/FrameGraph.hpp`.

`FrameGraph` is a backend-agnostic metadata, hazard-validation, and scheduling layer. It describes pass/resource intent, catches ordering mistakes, and compiles a stable execution plan; it does not allocate transient resources or emit Vulkan barriers.

Backend integration points:

- `engine/renderer/vulkan/VulkanRenderer.FrameResources.cpp` — contains startup graph construction; `VulkanRenderer::Impl` owns the graph state.
- `engine/renderer/vulkan/VulkanRenderer.Sync.cpp` — translates graph intent into Vulkan image layout/stage/access barriers.

## Scale and handles

- Pass/resource handles use wide opaque indices with `kInvalidIndex = 0xffffffff`.
- Pass, resource, and edge records are vector-backed; there is no fixed small graph cap.
- Handles remain stable because records are appended and never erased.
- Adding records throws only if the handle index range is exhausted or allocation fails.

Compilation indexes edges by pass and resource, so dependency construction scales with declared edges rather than rescanning the full edge list for every pass/resource pair.

## Enums

`FrameGraphResourceKind`:

- `Image`

`FrameGraphAccess`:

- `Read`
- `Write`

`FrameGraphUsage`:

- `ColorAttachment`
- `DepthAttachment`
- `SampledImage`
- `TransferSource`
- `Present`

## Handles

- `ResourceHandle`
- `PassHandle`

Each stores an internal index and has `valid()`. Passing an invalid or out-of-range handle throws.

## Descriptors

`ResourceDesc`:

- `name`
- `kind`
- `imported`
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

## Building a graph

```cpp
ve::FrameGraph graph;
auto depth = graph.addResource({.name = "Depth", .kind = ve::FrameGraphResourceKind::Image});
auto scene = graph.addPass({.name = "HDR Scene"});
graph.write(scene, depth, ve::FrameGraphUsage::DepthAttachment);
graph.compile();
```

Operations:

- `addResource(ResourceDesc) -> ResourceHandle`
- `addPass(PassDesc) -> PassHandle`
- `read(pass, resource, usage)`
- `write(pass, resource, usage)`
- `setFinalUsage(resource, usage)`

Adding the same `(pass, resource, access, usage)` edge twice throws immediately.

## Validation on `compile()`

`compile()` checks:

- every pass has at least one resource edge.
- non-imported resources are written before any pass reads them.
Compilation also derives RAW, WAR, and WAW hazard dependencies and exposes a stable topological pass order. The declaration index breaks ties, so independent passes remain deterministic.
For each resource with at least one edge, compilation also records the first and last pass in that execution order. These intervals are the input for future transient allocation and aliasing; the graph still does not own Vulkan memory.
It intentionally does not reject all multi-pass writes or resource reuse; future render-graph work needs those patterns.

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

## Current renderer use

The Vulkan backend caches separate graph topologies at startup:

- depth-prepass-on versus depth-prepass-off are constructed separately, so the no-prepass variant omits the HDR depth-read edge instead of filtering only the prepass node.
- screenshot-enabled versus screenshot-disabled are independent variant bits.
- `Auto` caches four combinations; `ForceOn` and `ForceOff` cache only the valid depth combinations.

Each frame selects the matching cached topology using the resolved depth-prepass state and whether a screenshot readback is active. Vulkan barriers and command recording remain explicit backend responsibilities; the selected graph supplies matching pass descriptors, final-usage metadata, and destination states for the migrated final-present, screenshot-readback, and HDR-sampling transitions. Live resource state remains the source of truth for barrier source scopes.

`BarrierIntent` is declarative synchronization metadata, not a Vulkan barrier. It records the selected resource usage, the previous usage when known, the destination pass (or an invalid pass for a final transition), and whether the intent is a final transition. Vulkan state mapping and command emission remain owned by `VulkanRenderer`.
