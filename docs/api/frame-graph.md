# Frame graph API

Header: `engine/renderer/FrameGraph.hpp`.

`FrameGraph` is currently a fixed-capacity metadata and validation layer. It describes pass/resource intent and catches simple ordering mistakes; it does not allocate transient resources or emit Vulkan barriers yet.

## Capacity

- `kMaxResources = 8`
- `kMaxPasses = 8`
- `kMaxEdges = 24`

All storage is internal fixed arrays. Capacity overflow throws `std::runtime_error`.

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

## Current renderer use

The Vulkan backend builds a static graph at startup for either:

- default path: HDR scene, tonemap/final, optional screenshot readback.
- depth-prepass path: depth prepass, HDR scene, tonemap/final, optional screenshot readback.

The graph supplies pass names/debug colors and validates that expected depth/screenshot edges exist. Vulkan image barriers still live in the renderer.
