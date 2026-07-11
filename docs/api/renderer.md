# Renderer API

Primary header: `engine/renderer/Renderer.hpp`. Backend integration: `engine/renderer/vulkan/VulkanRenderer.hpp`.

## `IRenderer`

Game-facing code depends on `IRenderer`:

```cpp
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void draw(const Camera&, const SceneRenderList&,
                      double sceneBuildMs, double elapsedSeconds,
                      double frameDeltaMs) = 0;
    [[nodiscard]] virtual RenderStats stats() const = 0;
    [[nodiscard]] virtual const RenderDeviceInfo& deviceInfo() const = 0;
};
```

`draw` borrows the camera and scene list only for the call. The caller retains ownership. Scene and timing inputs must be finite and satisfy the ranges documented in their headers.

`stats()` returns the latest published frame snapshot. `deviceInfo()` returns immutable capabilities chosen at startup.

## Device information

`RenderDeviceInfo` records adapter/API identity and the features that select runtime paths, including descriptor indexing, multi-draw indirect, first-instance indirect draws, timestamp support, upload queue choice, and masked-fragment support. Read the booleans as active capability decisions, not as a complete Vulkan feature dump.

## Statistics

`RenderStats` is grouped into:

- CPU scene-build, prepare, command-record, submit, and total durations;
- GPU frame, light, cull, shadow, depth, HDR, Hi-Z, and final-pass durations;
- draw, triangle, instance, mesh/cluster, visibility, and LOD counts;
- frame-graph passes, barriers, resources, transient slots/bytes, and recompiles;
- descriptor, light-list, shadow-atlas, probe, environment, and material-class pressure;
- asset cook/cache/reload state and active rendering modes.

GPU durations are usable only when `gpuTimestampsValid` is true. A disabled pass reports zero or unavailable timing according to the field contract. Counts describe the latest completed work where GPU readback is involved, so they may lag the CPU submission frame.

`sceneTriangleCount` is visible scene geometry before pass multiplication. `triangleCount` is submitted work across scene passes and fullscreen output. ImGui work is excluded.

## `VulkanRenderer`

`VulkanRenderer` is a final backend façade implementing `IRenderer`. It owns `VulkanRenderer::Impl`; copy and move are deleted. In addition to `draw`, `stats`, and `deviceInfo`, backend wiring may:

- request a PPM screenshot;
- publish a complete replacement authored-asset bundle;
- resolve mesh bounds/handles used by cooked-world instantiation;
- install an optional non-owning ImGui overlay callback;
- wait for idle during controlled teardown or integration work.

Do not expose `VulkanRenderer` or Vulkan handles in gameplay APIs. See [Renderer pipeline](../topics/renderer-pipeline.md) for backend behavior.

## Greedy meshing

`GreedyMesher.hpp` provides a CPU mesher for regular material volumes. Zero cells are empty; nonzero values are material IDs. It emits indexed quads, material draw ranges, counts, bounds, normals, UVs, and tangents. Optional neighbor planes control boundary culling. Optional per-column Y ranges must enclose all occupied cells.

The mesher is an offline/CPU utility; renderer backends do not invoke it implicitly.
