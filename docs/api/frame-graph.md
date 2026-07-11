# Frame graph API

Header: `engine/renderer/FrameGraph.hpp`.

`FrameGraph` compiles declared pass/resource access into execution order, barriers, logical lifetimes, and compatible transient slots. Native Vulkan allocation and command emission remain backend responsibilities.

## Model

- `ResourceHandle` and `PassHandle` are opaque indices.
- Resources are image or buffer, imported or transient.
- Pass edges declare read or write access plus a usage.
- Optional final usage describes state required after the last pass.
- Non-imported resources carry a transient byte estimate, alignment, and alias class.

Image usages include sampled, storage, color/depth attachment, present, and transfer. Buffer usages include storage, uniform, indirect, host-read, and transfer. Consult `FrameGraphUsage` for the complete set.

## Build

```cpp
ve::FrameGraph graph;
auto hdr = graph.addResource({
    .name = "hdr",
    .kind = ve::FrameGraphResourceKind::Image,
    .transientBytes = hdrBytes,
    .aliasClass = 1,
});

auto scene = graph.addPass({.name = "scene"});
graph.writeAttachment(scene, hdr, ve::FrameGraphUsage::ColorAttachment,
                      ve::FrameGraphAttachmentLoad::Clear,
                      ve::FrameGraphAttachmentStore::Store);
graph.setFinalUsage(hdr, ve::FrameGraphUsage::SampledImage);
graph.compile();
```

Graph mutation invalidates the compiled plan. `execute` and compiled queries require a successful compile.

## Compile validation

Compilation rejects:

- invalid handles or duplicate declarations;
- unsupported usage for the resource kind;
- read-only/write-only access mismatches;
- reads with no imported or prior written state;
- cycles and contradictory pass ordering;
- invalid final usage;
- incompatible alias metadata.

RAW, WAR, and WAW hazards produce dependencies. Declaration order breaks ties between otherwise independent passes. Each used resource receives first/last use, activation and retirement points, and barrier intents.

Transient resources may share a slot only when lifetimes do not overlap and kind plus nonzero alias class match. `TransientStats` reports requested bytes, physical slot bytes, aliased bytes, and slot count. This is a logical plan; it does not imply Vulkan memory aliasing unless the backend realizes it.

## Execution

`execute(callbacks, state)` walks the compiled plan and reuses `ExecutionState` storage after capacity is established. Lifecycle, transition, and pass callbacks are `noexcept` and report failure with `false`. Execution returns the failing phase/handle and retires active resources in reverse creation order.

The backend must make lifecycle callbacks transactional: activation failure must not publish a half-created binding, and retirement must tolerate failure unwind.

## Renderer use

The Vulkan backend caches graph variants for depth-prepass and screenshot combinations. Imported resources include swapchain images, temporal depth, and readback buffers. Graph order covers Forward+ assignment, visibility, shadow atlas, optional depth, HDR, depth-pyramid, tone-map/UI, readback, and final presentation/host-read transitions.

See [Renderer pipeline](../topics/renderer-pipeline.md) for native synchronization behavior.
