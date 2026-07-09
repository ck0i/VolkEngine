# Renderer API

Header: `engine/renderer/Renderer.hpp`. Advanced backend entry point: `engine/renderer/vulkan/VulkanRenderer.hpp`.

The renderer API is intentionally narrow. Game-facing code should depend on `IRenderer`, `RenderStats`, and `RenderDeviceInfo`; direct `VulkanRenderer` use is backend integration, not a general scene API.

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
| `descriptorIndexing`, `bindlessSampledImagesSupported` | Support-only metadata; bindless is not enabled yet. |
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
- `meshBatchCount`
- `drawCalls`
- `culledDrawCalls`
- `indirectSceneDraws`
- `triangleCount`

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

Use this directly only when wiring the Vulkan backend.

Public operations:

- `VulkanRenderer(Window& window, EngineConfig config)` — creates the backend and all renderer-owned Vulkan resources.
- `~VulkanRenderer()` — waits/tears down renderer resources and persists the pipeline cache during normal shutdown.
- copy construction/assignment are deleted.
- `draw(...)` — implements `IRenderer`.
- `stats()`, `deviceInfo()` — implement `IRenderer`.
- `requestScreenshot(std::filesystem::path)` — queues one screenshot for the next `draw()`.
- `waitIdle()` — explicit idle point for shutdown/test boundaries, not normal frame pacing.

Do not depend on `VulkanRenderer` private structs or Vulkan handles from game-facing code. Those details are free to change as the backend grows.
