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

## Current renderer use

The Vulkan backend builds one static graph at startup:

- auto/default path: a superset graph containing the depth prepass plus HDR scene depth read/write edges; command recording chooses the runtime path.
- forced-off path: HDR scene, tonemap/final, screenshot-readback metadata.
- forced-on path: depth prepass, HDR scene, tonemap/final, screenshot-readback metadata.

The `Screenshot Readback` pass/edge is always present so graph validation can prove the final swapchain image has a transfer-source path. The runtime image copy, fence wait, and PPM write are still conditional on `VulkanRenderer::requestScreenshot(path)`. Vulkan image barriers and transitions remain renderer responsibilities.
