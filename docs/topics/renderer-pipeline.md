# Renderer pipeline

This page describes backend runtime behavior and the current Vulkan implementation split.
Public type contracts remain in [Renderer API](../api/renderer.md), [Scene API](../api/scene.md), and [Frame graph API](../api/frame-graph.md).

For engine-facing code, prefer `IRenderer`; direct `VulkanRenderer` usage is backend integration and not required for normal app/game rendering flow.

## Source-map / ownership (current source split)

- `engine/renderer/Lighting.cpp` — backend-neutral validation, reference Forward+ tile assignment, deterministic shadow-atlas slot allocation, and CPU/GLSL ABI guards.
- `engine/renderer/vulkan/VulkanRenderer.hpp` — backend-specific public facade exposing `VulkanRenderer(Window&, EngineConfig)`, deleted copy/move, and methods:
  `draw`, `stats`, `deviceInfo`, `requestScreenshot`, `waitIdle`.
- `engine/renderer/vulkan/VulkanRenderer.cpp` — thin public wrapper that forwards every call to `VulkanRenderer::Impl`.
- `engine/renderer/vulkan/VulkanRendererImpl.hpp` — private implementation state and detail helpers (`Impl`, shared structs, constants, utility helpers, frame-graph and frame data declarations).
- `engine/renderer/vulkan/VulkanRenderer.Lifecycle.cpp` — startup/shutdown orchestration, constructor error rollback, `cleanupResources`, and swapchain-dependent setup/teardown entry points.
- `engine/renderer/vulkan/VulkanRenderer.Device.cpp` — Vulkan instance and debug-utils setup, surface creation, physical/logical-device setup, queue-family selection, queue creation, allocator creation, command pool bootstrap, and debug-object-name/debug-label helper functions.
- `engine/renderer/vulkan/VulkanRenderer.Swapchain.cpp` — swapchain capability queries, format/present-mode/extents choice, swapchain/image-view creation, and swapchain resize/recreate lifecycle for depth/HDR attachments.
- `engine/renderer/vulkan/VulkanRenderer.FrameResources.cpp` — per-frame resources, fences/semaphores, timestamp queries, and executable frame-graph variant construction/diagnostics.
- `engine/renderer/vulkan/VulkanRenderer.Resources.cpp` — long-lived GPU buffers/images, authored texture loading, generated HDR environment and shadow-atlas images/samplers, descriptor layouts/pools/sets, tonemap descriptor setup, and resource-registry metadata.
- `engine/renderer/vulkan/VulkanRenderer.Meshes.cpp` — procedural mesh construction, imported OBJ mesh loading, geometry buffer uploads, mesh-batch arrays, and `GpuMesh` offset/count helpers.
- `engine/renderer/vulkan/VulkanRenderer.Pipelines.cpp` — shader modules, pipeline layouts/pipelines, pipeline cache load/save/validation, and hot-reload path.
- `engine/renderer/vulkan/VulkanRenderer.Sync.cpp` — graph-usage to Vulkan synchronization mapping, tracked transitions, and rollback snapshots.
- `engine/renderer/vulkan/VulkanRenderer.Uploads.cpp` — staging uploads and transfer-queue versus same-queue synchronization.
- `engine/renderer/vulkan/VulkanRenderer.Visibility.cpp` — frustum extraction/culling, grid visibility acceleration, LOD bucketing, and draw-work planning.
- `engine/renderer/vulkan/VulkanRenderer.Lighting.cpp` — per-frame light/probe validation, tile-list capacity, Forward+ descriptor writes, practical directional cascade/local spot-shadow matrices, and bounded atlas preparation.
- `engine/renderer/vulkan/VulkanRenderer.Frame.cpp` — draw orchestration, graph execution callbacks, dynamic-rendering pass recording, submission/presentation, stats, and screenshot integration.
- `engine/renderer/vulkan/VulkanRenderer.ImGui.cpp` — optional built-in
  diagnostics or non-owning engine/editor overlay callback
  (`VOLKENGINE_ENABLE_IMGUI`) lifecycle and rendering.
- `engine/renderer/vulkan/VulkanRenderer.Screenshot.cpp` — screenshot request/readback handling, swapchain readback copy, PPM publishing, and temp/backup file behavior.
- `engine/renderer/vulkan/VmaUsage.cpp` — single translation unit containing `#define VMA_IMPLEMENTATION`.

`VulkanRenderer` startup is split across files but still follows this runtime sequence:

1. Create the instance and optional debug messenger.
2. Create the GLFW-backed surface, enumerate/rank physical devices, and select the adapter.
3. Create the logical device, queues, VMA allocator, debug-utils function pointers, and command pools.
4. Create the swapchain and image views, compile cached frame-graph variants, then transactionally realize depth/HDR targets and the fixed shadow atlas.
5. Create authored textures, the mipmapped linear HDR environment, samplers/descriptors, pipeline cache/pipelines, per-frame light/tile/shadow buffers, generated meshes, tonemap descriptors, and timestamp queries.
6. Create optional ImGui state, then log selected device capabilities and tracked resource totals.

`VulkanRenderer` enforces the contract: Vulkan 1.3, graphics/present/transfer queues, `VK_KHR_swapchain`, usable surface formats/present modes, dynamic rendering, synchronization2, and `shaderDemoteToHelperInvocation` for masked-material fragment behavior.
Startup logs include rejected adapters and concrete rejection reasons.

## Frame loop

Each frame executes the same high-level sequence:

1. Wait for the current frame fence and retire frame-owned/deferred resources.
2. Read the previous frame timestamp bucket when GPU timestamps are enabled.
3. Compute camera matrices and the CPU visibility plan, validate scene lighting/environment/probes, prepare bounded light-list/atlas storage and shadow cameras, grow mapped frame storage if required, update descriptors/uniforms, and reset the frame command pool.
4. Acquire a swapchain image.
5. Consume any pending screenshot request and begin optional ImGui work.
6. Select and execute the compiled graph variant into one primary command buffer.
7. Submit once to the graphics queue with the expected wait stages.
8. Present using the acquired image’s per-image wait semaphore and, when required, rebuild swapchain state.

Preparation that can be completed independently of a swapchain image runs before acquisition. If recording, allocation, ImGui preparation, or pre-submit bookkeeping fails after acquisition, the renderer restores tracked image state, releases the acquired image by recreating the swapchain, replaces the now-signaled per-frame acquire semaphore, and rethrows. A failed screenshot request is requeued unless a newer request has superseded it. This prevents reuse of a signaled binary semaphore and prevents an acquired image from being stranded.

Normal rendering does not call `vkDeviceWaitIdle`; the device-idle recovery path is reserved for swapchain recreation and exceptional post-acquire rollback.

## Render passes

Default adaptive path (`--auto-depth-prepass`, also `EngineConfig` default):

```mermaid
flowchart LR
    Decide[Runtime visible-count + triangle-count heuristic] -->|small scene| Scene[HDR Forward+ scene\ncolor + depth write]
    Decide -->|large scene| Depth[Depth prepass]
    Lights[Forward+ tile assignment] --> Scene
    Lights --> ScenePre
    Shadows[Directional cascades + local spot atlas] --> Scene
    Shadows --> ScenePre
    Depth --> ScenePre[HDR Forward+ scene\ndepth read + color write]
    Scene --> HiZ[Reverse-Z depth pyramid]
    ScenePre --> HiZ
    HiZ --> Tonemap[Exposure + ACES final pass]
    Tonemap --> ImGui[Optional ImGui overlay]
    ImGui --> Present[Present]
```

Forced no-prepass (`--no-depth-prepass`) selects a graph containing Forward+ assignment, visibility, shadow atlas, HDR depth-write, depth-pyramid, tonemap, and optional screenshot passes. Forced prepass (`--depth-prepass`) selects the equivalent graph with the depth-only pass and HDR depth-read. `--no-shadows` retains deterministic atlas state/graph synchronization but submits zero shadow views and skips shadow rendering.

`Auto` caches all depth/no-depth and screenshot/no-screenshot combinations and selects one after visibility planning; forced modes cache only their valid depth state. `FrameGraph::execute` owns pass order, logical resource activation/retirement, and barrier intent dispatch. Vulkan callbacks own physical bindings and command emission. The Forward+ compute write is visible to HDR fragment reads; shadow atlas depth writes transition to shader-read-only sampling before HDR. Depth remains reverse-Z: near maps to 1, far to 0, attachments clear to 0, and depth tests use `GREATER`/`GREATER_OR_EQUAL`.

The renderer uses Vulkan dynamic rendering (`vkCmdBeginRendering` / `vkCmdEndRendering`) rather than render-pass/framebuffer objects.
Swapchain images are preferred as UNORM because `tonemap.frag` normally applies exposure, ACES, and the standard sRGB OETF manually; if a surface only provides an sRGB swapchain format, the tonemap push constant disables shader-side OETF so Vulkan performs the single required encode.

### Forward+ lighting, shadows, and environment

- One compute workgroup covers each 16×16 screen tile, tests at most 256 point/spot lights in deterministic scene order, and writes a fixed 64-index partition plus exact overflow count. This avoids a global allocator and makes one dispatch sufficient for the complete screen.
- The fixed 2048² selected-depth-format atlas contains sixteen 512² slots. Three stable practical-split directional cascades receive camera-relative orthographic matrices; remaining slots are assigned to shadow-casting spot lights in scene order. Viewport/scissor are restored after atlas rendering before camera passes.
- Shadow sampling selects cascades by view-space distance, clamps projected depth/UV to the assigned interior, uses a slope-scaled receiver bias, and performs a fixed 3×3 PCF kernel through a comparison sampler. Masked materials use their authored base-color alpha/cutoff in both camera-depth and shadow variants.
- A renderer-owned 256×128 linear `R16G16B16A16_SFLOAT` equirectangular environment contains a bounded HDR sun and radiance-preserving CPU mip chain. Roughness selects specular LOD; diffuse uses the coarsest mip. Up to four spherical reflection probes blend tint/intensity within bounded radii against the same environment resource.
- Direct light uses GGX/Smith/Schlick. Specialized class branches add clear-coat, wrapped foliage/skin response, anisotropic hair, cloth sheen, and emissive output without new descriptor sets or pipeline permutations.

## Scene submission

- Generated and imported CPU meshes are triangle lists that keep full-float position/normal/uv/tangent data for import and tangent generation, then write compact Vulkan `GpuVertex` records directly into one mapped staging buffer: full-float position/UV plus SNORM16 normal/tangent attributes. All batches share one vertex buffer and one index buffer; indices are reordered for post-transform vertex-cache locality, then vertices are remapped to first-use order for vertex-fetch locality while `loadObjMesh()` keeps source OBJ fan order until upload.
- `GpuMesh` records carry offset/count values only.
- `SceneRenderItem` records carry mesh ID, model matrix, material constants, and bounds. The GPU path uploads compact cull candidates and instance records; the direct fallback retains CPU materialization.
- Capability-gated bindless scene descriptors use stable texture-table indices for albedo, normal, and ORM roles. Devices without the required descriptor-indexing features retain fixed material descriptor sets.
- Meshes are cooked into bounded clusters with bounds and per-mesh ranges. A compute pass performs instance/cluster frustum tests, sphere LOD selection, temporal depth-pyramid occlusion, visible-instance compaction, and indirect-command generation.
- The temporal Hi-Z pass reads the previous completed pyramid during culling, renders the current depth/HDR work, then conservatively reduces current reverse-Z depth into a half-resolution R32 pyramid for the next submission. Proportional footprints retain odd-extent edge texels.
- The frame graph declares cull buffers and the depth pyramid explicitly; same-queue submission order and graph barriers carry the temporal image from the current build to the next frame's read.
- `vkCmdDrawIndexedIndirect` submits the generated cluster commands when `multiDrawIndirect`, `drawIndirectFirstInstance`, and `maxDrawIndirectCount` allow it. `--no-indirect-draws` selects the direct indexed fallback.

## Swapchain and resize

- `--vsync` selects FIFO.
- `--no-vsync` prefers immediate, then mailbox, then FIFO.
- Resize/minimize waits for a non-zero framebuffer extent and returns if the window closes while minimized.
- Swapchain recreation rebuilds image views, per-image render-finished semaphores, depth/HDR images, and tonemap/ImGui state.
- Pipelines are recreated when dependent formats change; otherwise existing pipelines are reused. Recreate/teardown paths detach active pipeline handles into a `PipelineSet` before destruction so member handles are nulled before any cleanup or error path can observe them.

## Screenshot path

`VulkanRenderer::requestScreenshot(path)` queues one request with latest-request-wins coalescing while a request is pending. The next `draw()` consumes it, records an image-to-buffer transfer copy from the final swapchain image when supported, and waits on the submitting frame before writing disk. A failure before queue submission returns the consumed path to the pending slot unless a newer request superseded it.

Output is complete-before-publish:

- writes binary PPM (P6) via a temporary file,
- renames into place atomically when possible,
- falls back to backup/restore if target replacement is restricted by platform semantics.

Unsupported format/usage combinations (no `TRANSFER_SRC` support or non-UNORM swapchain format) are reported and skipped safely.

## Debug and diagnostics

- Debug-utils names are assigned to long-lived Vulkan objects when available.
- Pass regions are labeled for RenderDoc/validation captures.
- `RenderStats` exposes CPU timing buckets; light-assignment/cull/shadow/depth/HDR/Hi-Z/final GPU intervals; draw/triangle/visibility state; light-list and shadow-atlas pressure; probes/environment/exposure; material-class counts; and submission mode.
- `RenderDeviceInfo` mirrors adapter, Vulkan 1.3 masked-fragment capability, descriptor/indirect features, and upload-sync decisions.
- ImGui and run-summary schema v5 expose bounded lighting/material state; the
  summary additionally records bounded job/IO counters and worker timing.
  A configured overlay callback replaces the built-in diagnostics window while
  reusing the same ImGui frame and graph pass. `--no-imgui` skips overlay
  initialization and callback work.
