# Performance model

VolkEngine's Vulkan renderer keeps public API in `VulkanRenderer.hpp` and implements measured hot paths across private `VulkanRenderer::Impl` split files. The exhaustive split map lives in [Renderer pipeline](renderer-pipeline.md#source-map--ownership-current-source-split); this page names only performance-sensitive owners.

## Performance-sensitive owners

- `engine/renderer/vulkan/VulkanRenderer.Frame.cpp`: `draw` orchestration, swapchain acquire/present, exception-safe command-buffer submission, and screenshot-in-frame integration.
- `engine/renderer/vulkan/VulkanRenderer.FrameResources.cpp`: per-frame command pools/buffers, mapped uniform/instance buffers, mapped indirect buffers when indirect submission is active, fences/semaphores, timestamp query pool, and frame graph compilation state.
- `engine/renderer/vulkan/VulkanRenderer.Visibility.cpp`: `SceneVisibilityPlan`, frustum culling, mesh-bucket accounting, material-grid tile acceleration, and temporal cache replay.
- `engine/renderer/vulkan/VulkanRenderer.Uploads.cpp`: one-shot upload command submission, upload queue tracking, completion fences, and queue-semaphore coordination.
- `engine/renderer/vulkan/VulkanRenderer.Sync.cpp`: image layout/state transitions, barrier construction, and per-frame sync snapshots for recovery.
- `engine/renderer/vulkan/VulkanRenderer.Pipelines.cpp`: shader module validation/load, pipeline layouts/pipelines, pipeline cache validation/publish, and shader hot-reload rebuild/retire rules.
- `engine/renderer/vulkan/VulkanRenderer.Resources.cpp`: long-lived texture/image/buffer resources, samplers, descriptor layouts/pools/sets, and resource registry accounting.
- `engine/renderer/vulkan/VulkanRenderer.Meshes.cpp`: generated cube/sphere/plane geometry, imported OBJ geometry, bounds propagation, and shared device-local vertex/index uploads.
- `engine/renderer/vulkan/VulkanRenderer.Screenshot.cpp`: screenshot request gating, image-to-buffer transfer setup, PPM writing, and atomic publish semantics.
- `engine/renderer/vulkan/VulkanRenderer.ImGui.cpp`: optional diagnostics overlay lifecycle, periodic diagnostics refresh, and ImGui draw-data emission.

## Hot-path rules

- Do not allocate per frame unless a device limit or visible-count growth requires it.
- Prefer persistent mapping for tiny CPU-to-GPU frame data.
- Keep staging uploads outside the normal frame loop.
- Submit one primary frame command buffer and one graphics queue submit per frame.
- Use dynamic rendering (`vkCmdBeginRendering`) for the in-frame depth/HDR/tonemap sequence.
- Reset per-frame command pools after their fences signal.
- Treat optional diagnostics (`--no-imgui`, `--no-gpu-timestamps`) as benchmark controls, not error paths.
- Avoid `vkDeviceWaitIdle` in normal rendering.

## Implemented performance features

| Area | Current behavior | Module owner |
| --- | --- | --- |
| Frames in flight | Two frame slots decouple CPU prep from GPU completion without unbounded latency. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp` |
| Presentation | `--no-vsync` prefers immediate mode for lowest present-queue latency when available. Throw-prone scene preparation runs before image acquisition; exceptional post-acquire/pre-submit failures recreate the swapchain and replace the signaled acquire semaphore before rethrowing. Acquired-image sync state chains the semaphore wait into the graph's first swapchain transition. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp`, `VulkanRenderer.Swapchain.cpp` |
| Depth prepass | Default `Auto` mode selects a cached graph variant using visible item/triangle thresholds with hysteresis; forced prepass uses a dedicated depth-only vertex shader with a position-only vertex input, while `--depth-prepass` and `--no-depth-prepass` force either path for comparisons. Read-only depth attachments use `VK_ATTACHMENT_STORE_OP_NONE`, preventing discard policy from introducing a false cross-frame write. The camera projection is reverse-Z (near maps to depth 1, far to 0), so depth clears use 0 and scene/depth pipelines use `GREATER`/`GREATER_OR_EQUAL` tests. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp`, `VulkanRenderer.Pipelines.cpp` |
| Geometry | Generated and imported meshes are packed directly into one mapped staging buffer before upload to shared device-local vertex/index buffers, then full-float CPU mesh arrays are released; OBJ import first counts records to reserve attribute, vertex, index, face scratch, and lookup storage with renderer-range guards before the real parse, treats degenerate explicit normals as missing so generated normals prevent shader NaNs, skips scale-relative degenerate face triangles before index emission, and compacts skipped-only vertices before tangent/bounds calculation. CPU meshes keep full-float tangent-basis data only through import/tangent generation, while triangle-indexed tangent generation validates index ranges once before accumulation instead of branching per triangle. The Vulkan vertex stream packs normal/tangent attributes as `R16G16B16A16_SNORM` to cut vertex bandwidth. Renderer upload reorders triangle-list indices for post-transform vertex-cache locality, remaps vertices into first-use order for vertex-fetch locality, and derives imported-model scene bounds from loaded mesh bounds. | `VulkanRenderer.Meshes.cpp`, `VulkanRenderer.Pipelines.cpp`, `Geometry.cpp` |
| Greedy meshing | Per-invocation face-visit scratch uses six packed `uint64_t` bitplanes (one bit per cell and face), reducing temporary state from 24 bytes per cell to 0.75 bytes per cell while preserving material-aware quad merging and boundary-neighbor behavior. | `GreedyMesher.cpp` |
| Scene instances | Per-frame mapped storage starts at 2048 records and grows transactionally after its fence. The GPU path writes compact cull candidates, compacts visible instances in compute, and keeps CPU instance materialization only for the explicit direct fallback. | `VulkanRenderer.FrameResources.cpp`, `VulkanRenderer.Frame.cpp`, `VulkanRenderer.Visibility.cpp` |
| Visibility | Mesh upload cooks bounded clusters and ranges. Compute performs instance/cluster frustum tests, sphere LOD selection, conservative temporal Hi-Z rejection, visible-instance compaction, counters, and command generation. A half-resolution reverse-Z min pyramid retains odd source edges with proportional footprints and reduced owned-image memory by about 4 MiB at 1280×720 versus a full-resolution base. | `VulkanRenderer.Visibility.cpp`, `VulkanRenderer.Frame.cpp`, `depth_pyramid.comp`, `scene_cull.comp` |
| Submission | The GPU path emits per-cluster indexed indirect commands and visible-instance indices, then submits them with one `vkCmdDrawIndexedIndirect` per scene pass. The direct indexed fallback remains capability/config gated and does not allocate cull buffers. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp` |
| Texture upload | Startup material textures are loaded transactionally, packed into one shared staging buffer, release decoded CPU mip pixels immediately after staging copy, and submit once on the graphics upload path; explicit mip policies keep albedo gamma-correct and alpha-weighted only when needed, keep ORM/linear scalar CPU fallback as straight RGBA averages, and keep normal maps CPU-renormalized, while staging resources remain alive until the upload fence retires. | `VulkanRenderer.Resources.cpp`, `VulkanRenderer.Uploads.cpp`, `ImageLoader.cpp` |
| Uploads | Same-queue uploads use in-command transfer barriers (`VkBufferMemoryBarrier2`) after packed mesh/texture staging copies; steady frames without upload semaphores point `vkQueueSubmit` directly at the swapchain image-available semaphore, while separate transfer queue uploads inject per-upload signal semaphores consumed by the fallback frame-submit wait list. That fallback list now reserves frame ownership capacity before `vkQueueSubmit`, so queued upload semaphores cannot be orphaned by a post-submit allocation failure. | `VulkanRenderer.Uploads.cpp`, `VulkanRenderer.Meshes.cpp`, `VulkanRenderer.Frame.cpp` |
| Command buffers | One primary command buffer per frame; shared scene vertex/index/descriptor bindings are recorded once before scene passes, per-frame command pools are transient and reset as a unit, and command buffers are recorded for one-time submit. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp` |
| Frame graph | Cached depth/no-depth and screenshot/no-screenshot variants execute cluster cull, depth, HDR, temporal depth-pyramid build, tonemap/ImGui, and readback work directly. Imported temporal resources preserve read-before-write order; compilation derives deterministic barriers, lifetimes, activation/retirement points, and alias-compatible transient slots. | `FrameGraph.hpp`, `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp` |
| Timestamps | Fixed timestamp ranges report frame, GPU cluster cull, depth, HDR, depth-pyramid, and final-pass durations; `gpuTimestampsValid` distinguishes completed query data from pending/unavailable values. | `VulkanRenderer.FrameResources.cpp` |
| Pipeline cache / creation | Header/device validated cache load/save at `${binaryDir}/cache/pipeline_cache.bin` with temp-file publish and post-readback validation; depth-prepass, prepass-aware scene, and no-prepass scene pipelines are created in one batched Vulkan call. | `VulkanRenderer.Pipelines.cpp` |
| Resource accounting | Vector-backed registry reports live renderer-owned/imported estimated bytes without owning memory or imposing a fixed small resource cap. | `VulkanRenderer.Resources.cpp`, `GpuResourceRegistry.hpp` |
| Diagnostics | Optional overlay and schema-v2 run summaries expose graph structure/allocation, recompiles, per-pass timings, descriptor pressure, and cooked/visible/tested/occluded cluster counts. | `VulkanRenderer.ImGui.cpp`, `RunSummary.cpp` |
| Dynamic rendering | Graph callbacks record cluster cull, optional depth, HDR, temporal Hi-Z build, tonemap/ImGui, and screenshot copy. Tracked image/buffer state skips exact no-op transitions; graph final transitions cover presentation and host readback. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.Sync.cpp` |

## Reading `RenderStats`

`cpuFrameMs` is the renderer submit window: visibility/capacity preparation through queue-submit bookkeeping, including swapchain acquisition and screenshot/overlay setup. It excludes present pacing, resize handling, and screenshot readback waits.

CPU bucket fields are mutually exclusive:

- `cpuSceneBuildMs` — demo scene-list production.
- `cpuPrepareMs` — visibility planning, mesh lookup, culling, capacity growth, swapchain acquisition, screenshot preparation, and overlay stats refresh.
- `cpuCommandRecordMs` — instance/indirect materialization, stat derivation, ImGui draw-data encoding, and Vulkan command recording.
- `cpuQueueSubmitMs` — queue submit setup and submission bookkeeping.

GPU timing fields are valid only when `gpuTimestampsValid` is true. `gpuCullMs` measures generated visibility work before scene rendering and `gpuDepthPyramidMs` measures the post-HDR pyramid build. When the prepass is disabled by `Auto` or `--no-depth-prepass`, `gpuDepthPrepassMs` is zero and HDR timing includes the depth-writing scene pass.

Draw stats intentionally exclude ImGui draw lists. Scene draw counts include scene submissions plus the fullscreen tonemap draw; `sceneTriangleCount` is visible scene geometry before pass multiplication, while `triangleCount` is submitted triangle work including depth/HDR scene passes and the fullscreen tonemap triangle.

## Benchmark switches

Use sandbox flags to isolate costs:

- `--no-vsync` — minimize present pacing.
- `--no-imgui` — remove overlay setup, diagnostics refresh, and draw data.
- `--no-gpu-timestamps` — remove query pool and timestamp writes.
- `--no-indirect-draws` — force direct draw fallback.
- `--auto-depth-prepass` / `--depth-prepass` / `--no-depth-prepass` — compare adaptive, forced prepass, and depth-writing HDR paths.
- `--grid-rows N --grid-columns N` — scale scene-list size.
- `--grid-tile-rows N --grid-tile-columns N` — measure material-grid tile granularity.
- `--frames N --resize-smoke` — repeatable automated smoke.

## Known next work

- Establish representative geometry/material crossover gates for the GPU-generated path and temporal Hi-Z across discrete and integrated drivers.
- Resource residency detail and allocation statistics by resource class.
- GPU-native compressed texture formats and streaming upload queues.
- Map compatible transient slots to reusable aliased Vulkan memory only when real graph resources create measurable overlap savings.
- Add hierarchy traversal and finer cluster LOD selection only after the flat bounded-cluster benchmark establishes the required crossover.

## External references used for current choices

- Khronos wait-idle guidance: <https://docs.vulkan.org/samples/latest/samples/performance/wait_idle/README.html>
- Khronos command-buffer recycling guidance: <https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html>
- Khronos multi-draw indirect sample: <https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/multi_draw_indirect/README.adoc>
- NVIDIA Vulkan dos and don'ts: <https://developer.nvidia.com/blog/vulkan-dos-donts/>
- Vulkan present modes: <https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html>
- Vulkan feature bits: <https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html>
- Vulkan pipeline cache guide: <https://docs.vulkan.org/guide/latest/pipeline_cache.html>
- Vulkan synchronization examples: <https://docs.vulkan.org/guide/latest/synchronization_examples.html>
