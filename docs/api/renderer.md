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
    virtual void draw(const Camera& camera, const SceneRenderList& scene,
                      double sceneBuildMs, double elapsedSeconds, double frameDeltaMs) = 0;
    [[nodiscard]] virtual RenderStats stats() const = 0;
    [[nodiscard]] virtual const RenderDeviceInfo& deviceInfo() const = 0;
};
```
Submits one frame using camera state, a caller-owned `SceneRenderList`, and frame timing. The list is borrowed synchronously for the duration of the call and is never retained by the renderer. Current concrete behavior includes visibility planning, instance/indirect materialization, command recording, submit, and present.

`sceneBuildMs` reports the caller’s scene-list construction time for telemetry. It must be finite and non-negative. The renderer includes it in `cpuFrameMs` and publishes it as `cpuSceneBuildMs`; `cpuPrepareMs` and later buckets remain renderer-side timings.

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
| `descriptorIndexing`, `bindlessSampledImagesSupported` | Descriptor-indexing features and the capability-gated runtime sampled-image table. Unsupported devices retain the fixed material-set fallback. |
| `multiDrawIndirect`, `drawIndirectFirstInstance` | Feature bits required for the indirect scene path. |
| `samplerAnisotropy`, `maxSamplerAnisotropy` | Texture sampler capability actually enabled/selected. |
| `shaderDemoteToHelperInvocation` | Vulkan 1.3 masked-material capability selected and enabled; adapters lacking it are rejected rather than compiling an invalid alpha-tested shader path. |
| `transferUploadSync` | `SameQueueBarrier` or `QueueSemaphore` upload first-use path. |
| `indirectSceneDraws` | Whether indirect scene submission is active, not just requested. |

## `RenderStats`

Timing fields:

- `cpuFrameMs`: render-submit CPU window.
- `cpuSceneBuildMs`, `cpuPrepareMs`, `cpuCommandRecordMs`, `cpuQueueSubmitMs`: exclusive CPU buckets inside `cpuFrameMs`.
- `frameDeltaMs`: wall-clock frame delta supplied by the app loop.
- `gpuFrameMs`, `gpuLightAssignmentMs`, `gpuCullMs`, `gpuShadowMs`, `gpuDepthPrepassMs`, `gpuHdrSceneMs`, `gpuDepthPyramidMs`, `gpuFinalPassMs`: timestamp-derived GPU intervals when valid. Disabled shadow work remains unavailable in schema telemetry rather than presenting a measured pass.
- `gpuTimestampsValid`: true only after completed query data was read.
- `depthPyramidBuildEnabled`, `depthPyramidOcclusion`: distinguish an executing pyramid build from active use by culling; CPU-reference validation builds the pyramid while intentionally disabling occlusion.
- `elapsedSeconds`: frame time source used for animation.

Frame-graph fields:

- `cpuGraphCompileMs`: last cached-variant compilation duration.
- `graphPassCount`, `graphResourceCount`, `graphBarrierCount`: selected variant structure.
- `graphPhysicalAllocationCount`: compiled transient slot count.
- `graphTransientRequestedBytes`, `graphTransientAllocatedBytes`: logical demand versus slot capacity.
- `graphRecompileCount`, `graphLastCompileWasResize`: compile frequency and most recent ownership event.

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
- `culledItemCount`: rejected scene items/instances; this is distinct from `drawCalls`, which counts submitted mesh commands.
- `indirectSceneDraws`
- `sceneTriangleCount`: visible scene geometry before multiplying by depth/HDR scene passes.
- `triangleCount`: submitted triangle work, preserving scene-pass multiplication and the fullscreen tonemap triangle.
- `sceneClusterCount`: number of cooked geometry clusters.
- `visibleCullingUnitCount`, `testedCullingUnitCount`, `occludedCullingUnitCount`: completed GPU visibility workload/rejection counters.
- `cullingUnitsAreClusters`: counter granularity; `false` means scene instances in mesh-command mode, while `true` means cluster instances in cluster-command mode.
- `materialDescriptorCount`, `materialDescriptorCapacity`: live bindless sampled-image pressure, or fixed-fallback occupancy/capacity.

Lighting/material fields:

- `localLightCount`, `lightListOverflowCount`: submitted bounded light count and completed Forward+ tile-index overflow.
- `shadowViewCount`, `shadowAtlasCapacity`, `shadowAtlasOverflowCount`: active directional/local atlas views, fixed slot capacity, and deterministic slot pressure.
- `reflectionProbeCount`: active bounded spherical probes.
- `materialClassCounts[8]`: visible submitted instance counts indexed by the canonical `RenderMaterialClass` ABI.
- `shadowsEnabled`, `environmentMapEnabled`, `effectiveExposure`: actual renderer path and frame exposure after scene compensation.

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

- `VulkanRenderer(Window& window, EngineConfig config, ReferenceAssetBundle& referenceAssets)` — constructs backend state, initializes renderer-owned resources, and borrows the stable application-owned active bundle for renderer lifetime.
- `~VulkanRenderer()` — destroys backend state via `Impl` teardown and persists pipeline-cache metadata during normal shutdown.
- copy construction/assignment are deleted.
- move construction/assignment are deleted.
- `draw(...)` — implements `IRenderer`.
- `meshBounds(MeshAssetHandle)` — returns renderer-owned local-space bounds for a generational mesh handle; application scene producers can seed conservative item bounds without accessing Vulkan internals.
- `materialTextureHandles(AssetId)` — resolves the active authored material's base-color, normal, and metallic-roughness texture handles.
- `stats()`, `deviceInfo()` — implement `IRenderer`.
- `reloadReferenceAssets(ReferenceAssetBundle candidate)` — main-thread publication boundary for a fully cooked candidate. Mesh/cluster/texture resources and dependent descriptors replace the old set transactionally after GPU idle; failure restores the old GPU resources, descriptors, stats, and active bundle.
- `requestScreenshot(std::filesystem::path)` — queues one screenshot for the next `draw()`.
- `armAcquireRecoverySmoke()` — diagnostics-only one-shot fault injection used by `RunOptions::acquireRecoverySmoke`; ordinary game-facing code should not call it.
- `waitIdle()` — explicit idle synchronization point for shutdown/test boundaries, not for normal pacing.

Game-facing code must stay at the `IRenderer` level and `RenderStats`/`RenderDeviceInfo` contracts.
Do not rely on `VulkanRendererImpl.hpp`, private `Impl` members, or Vulkan handles from callers.
