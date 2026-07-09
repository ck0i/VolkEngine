# Renderer API

Header: `engine/renderer/Renderer.hpp`. Advanced backend entry point: `engine/renderer/vulkan/VulkanRenderer.hpp`.

The renderer API is intentionally narrow. Game-facing code should depend on `IRenderer`, `RenderStats`, and `RenderDeviceInfo`; direct `VulkanRenderer` use is backend integration, not a general scene API.

## `GreedyMeshingVolume`

`engine/renderer/GreedyMesher.hpp` exposes a CPU surface mesher for regular
cell volumes; renderer backends do not invoke it directly. Clients supply
cells using Y-major indexing (`y + x * height + z * width * height`); zero is
empty and non-zero values are material IDs. `generateGreedyMesh()` mirrors
the reference run strategy: it scans heightmap-bounded columns, culls occupied
neighbors, greedily extends each visible face along two axes, and emits indexed
four-vertex quads. It also returns contiguous material draw ranges, visible-face/quad counts, bounds, and renderer-compatible normals, UVs, and tangents.
Boundary planes are supplied in `-X,+X,-Y,+Y,-Z,+Z` order; missing neighbors follow `meshExterior`, while
`emitBoundaryFaces` explicitly preserves finite-grid boundary faces.
Optional `minY`/`maxY` spans cache per-column bounds in `x + z*width` order;
they must enclose every non-zero cell and use `minY=height`, `maxY=0` for
empty columns. Omit both to build the bounds internally.

## `IRenderer`

```cpp
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void draw(const Camera& camera, double elapsedSeconds, double frameDeltaMs) = 0;
    [[nodiscard]] virtual RenderStats stats() const = 0;
    [[nodiscard]] virtual const RenderDeviceInfo& deviceInfo() const = 0;
};
```

### `draw`

Submits one frame using camera state and frame timing. Current concrete behavior includes scene-list build, visibility planning, instance/indirect materialization, command recording, submit, and present.

### `stats`

Returns a copy of the most recently published frame stats. Treat GPU timing fields as valid only when `gpuTimestampsValid` is true.

### `deviceInfo`

Returns immutable backend/device metadata selected during initialization.

## `RenderDeviceInfo`

| Field | Meaning |
| --- | --- |
| `backend` | Current backend enum; only `RenderBackend::Vulkan` exists. |
| `adapterName` | Selected physical device name. |
| `apiVersionMajor/Minor/Patch` | Vulkan API version reported by the adapter. |
| `maxImageDimension2D` | Device limit. |
| `maxDrawIndirectCount` | Device limit used by indirect scene submission. |
| `discreteGpu` | Adapter type classification. |
| `dynamicRendering`, `synchronization2` | Required renderer feature bits. |
| `timestampQueries` | Timestamp path enabled after support/config checks. |
| `validationEnabled`, `debugMarkers` | Diagnostics state. |
| `memoryBudget` | VMA memory budget support. |
| `descriptorIndexing`, `bindlessSampledImagesSupported` | Descriptor-indexing features enabled when supported; the current material path uses a fixed combined-sampler descriptor array for albedo, normal, and ORM textures, with future resource tables able to grow from that shape. |
| `multiDrawIndirect`, `drawIndirectFirstInstance` | Feature bits required for the indirect scene path. |
| `samplerAnisotropy`, `maxSamplerAnisotropy` | Texture sampler capability actually enabled/selected. |
| `transferUploadSync` | `SameQueueBarrier` or `QueueSemaphore` upload first-use path. |
| `indirectSceneDraws` | Whether indirect scene submission is active, not just requested. |

## `RenderStats`

Timing fields:

- `cpuFrameMs`: render-submit CPU window.
- `cpuSceneBuildMs`, `cpuPrepareMs`, `cpuCommandRecordMs`, `cpuQueueSubmitMs`: exclusive CPU buckets inside `cpuFrameMs`.
- `frameDeltaMs`: wall-clock frame delta supplied by the app loop.
- `gpuFrameMs`, `gpuDepthPrepassMs`, `gpuHdrSceneMs`, `gpuFinalPassMs`: timestamp-derived GPU intervals when valid.
- `gpuTimestampsValid`: true only after completed query data was read.
- `elapsedSeconds`: frame time source used for animation.

Submission/scene fields:

- `depthPrepassEnabled`
- `scenePassCount`
- `sceneItemCount`
- `visibleItemCount`
- `sceneInstanceCapacity`
- `sceneInstanceBufferMiB`
  - This reflects the current private instance-record layout; it includes model, normal-matrix, and material data.
- `meshBatchCount`
- `drawCalls`
- `culledDrawCalls`
- `indirectSceneDraws`
- `sceneTriangleCount`: visible scene geometry before multiplying by depth/HDR scene passes.
- `triangleCount`: submitted triangle work, preserving scene-pass multiplication and the fullscreen tonemap triangle.

Grid/LOD fields:

- `gridTileCount`
- `gridTilesCulled`
- `gridTilesAccepted`
- `gridTilesIntersected`
- `gridVisibilityCacheHit`
- `gridVisibilityWorkItems`
- `sphereLodHighCount`
- `sphereLodMediumCount`
- `sphereLodLowCount`

Draw counts exclude ImGui overlay draw lists.

## Advanced: `VulkanRenderer`

`VulkanRenderer` is the backend-specific Vulkan facade for engine integration only.
It is a thin PIMPL wrapper that owns a private `VulkanRenderer::Impl` and forwards all renderer behavior to it.

Implementation map:

The authoritative Vulkan file-role map lives in [Renderer pipeline](../topics/renderer-pipeline.md#source-map--ownership-current-source-split). API docs only depend on these boundaries:

- `engine/renderer/vulkan/VulkanRenderer.hpp` — public facade declaration.
- `engine/renderer/vulkan/VulkanRenderer.cpp` — forwarding wrapper.
- `engine/renderer/vulkan/VulkanRendererImpl.hpp` — private implementation state and helper declarations.

Current public operations:

- `VulkanRenderer(Window& window, EngineConfig config)` — constructs backend state and initializes renderer-owned resources.
- `~VulkanRenderer()` — destroys backend state via `Impl` teardown and persists pipeline-cache metadata during normal shutdown.
- copy construction/assignment are deleted.
- move construction/assignment are deleted.
- `draw(...)` — implements `IRenderer`.
- `stats()`, `deviceInfo()` — implement `IRenderer`.
- `requestScreenshot(std::filesystem::path)` — queues one screenshot for the next `draw()`.
- `waitIdle()` — explicit idle synchronization point for shutdown/test boundaries, not for normal pacing.

Game-facing code must stay at the `IRenderer` level and `RenderStats`/`RenderDeviceInfo` contracts.
Do not rely on `VulkanRendererImpl.hpp`, private `Impl` members, or Vulkan handles from callers.
