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
| Presentation | `--no-vsync` prefers immediate mode for lowest present-queue latency when available. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.Swapchain.cpp` |
| Depth prepass | Default `Auto` mode compiles a static frame-graph superset and uses visible item/triangle thresholds with hysteresis; forced prepass uses a dedicated depth-only vertex shader with a position-only vertex input, while `--depth-prepass` and `--no-depth-prepass` force either path for comparisons. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp`, `VulkanRenderer.Pipelines.cpp` |
| Geometry | Generated and imported meshes are packed directly into one mapped staging buffer before upload to shared device-local vertex/index buffers, then full-float CPU mesh arrays are released; OBJ import first counts records to reserve attribute, vertex, index, face scratch, and lookup storage with renderer-range guards before the real parse, skips scale-relative degenerate face triangles before index emission, and compacts skipped-only vertices before tangent/bounds calculation. CPU meshes keep full-float tangent-basis data only through import/tangent generation, while triangle-indexed tangent generation validates index ranges once before accumulation instead of branching per triangle. The Vulkan vertex stream packs normal/tangent attributes as `R16G16B16A16_SNORM` to cut vertex bandwidth. Renderer upload also reorders triangle-list indices for post-transform vertex-cache locality, and imported-model scene bounds come from loaded mesh bounds. | `VulkanRenderer.Meshes.cpp`, `VulkanRenderer.Pipelines.cpp`, `Geometry.cpp` |
| Scene instances | Per-frame mapped storage buffer starts at 2048 visible instances, packs CPU-precomputed normal-matrix columns for non-uniform-scale TBN correctness, and grows transactionally after the frame fence so the old mapped descriptor remains valid until the replacement buffer is mapped and descriptors are updated. | `VulkanRenderer.FrameResources.cpp`, `VulkanRenderer.Frame.cpp` |
| Visibility | CPU frustum culling, mesh-bucket counts, material-grid tile acceleration, temporal static-grid visibility cache, and range-split instance materialization so only grid-tile cache population pays the cache-write branch. | `VulkanRenderer.Visibility.cpp`, `VulkanRenderer.Frame.cpp` |
| Submission | Per-mesh instances are materialized in CPU scratch with one `{depth,index}` sort key emitted at the same time per local instance, sorted front-to-back through compact key arrays, copied sequentially to the mapped instance buffer, then submitted with multi-draw indirect when required Vulkan feature bits are enabled; direct indexed-instanced fallback avoids allocating per-frame indirect command buffers. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp` |
| Texture upload | Startup material textures are loaded transactionally, packed into one shared staging buffer, release decoded CPU mip pixels immediately after staging copy, and submit once on the graphics upload path; albedo uses GPU blit mips when available and falls back to gamma-correct CPU mip generation when sRGB linear blits are unsupported, while normal maps upload CPU-renormalized mip chains, bind a separate non-anisotropic sampler over their explicit mip range, and keep staging resources alive until the upload fence retires. | `VulkanRenderer.Resources.cpp`, `VulkanRenderer.Uploads.cpp`, `ImageLoader.cpp` |
| Uploads | Same-queue uploads use in-command transfer barriers (`VkBufferMemoryBarrier2`) after packed mesh/texture staging copies; steady frames without upload semaphores point `vkQueueSubmit` directly at the swapchain image-available semaphore, while separate transfer queue uploads inject per-upload signal semaphores consumed by the fallback frame-submit wait list. That fallback list now reserves frame ownership capacity before `vkQueueSubmit`, so queued upload semaphores cannot be orphaned by a post-submit allocation failure. | `VulkanRenderer.Uploads.cpp`, `VulkanRenderer.Meshes.cpp`, `VulkanRenderer.Frame.cpp` |
| Command buffers | One primary command buffer per frame; shared scene vertex/index/descriptor bindings are recorded once before scene passes, per-frame command pools are transient and reset as a unit, and command buffers are recorded for one-time submit. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp` |
| Timestamps | Fixed timestamp range per frame slot; `gpuTimestampsValid` distinguishes real timings from pending/unavailable data. | `VulkanRenderer.FrameResources.cpp` |
| Pipeline cache / creation | Header/device validated cache load/save at `${binaryDir}/cache/pipeline_cache.bin` with temp-file publish and post-readback validation; depth-prepass, prepass-aware scene, and no-prepass scene pipelines are created in one batched Vulkan call. | `VulkanRenderer.Pipelines.cpp` |
| Resource accounting | Vector-backed registry reports live renderer-owned/imported estimated bytes without owning memory or imposing a fixed small resource cap. | `VulkanRenderer.Resources.cpp`, `GpuResourceRegistry.hpp` |
| Diagnostics | Optional overlay is controlled by `--no-imgui` and runs only when `debugOverlay` is enabled; it refreshes stats snapshots at a fixed interval. | `VulkanRenderer.ImGui.cpp` |
| Dynamic rendering | Depth pass (optional), HDR scene pass, and tonemap pass are rendered with dynamic rendering (`vkCmdBeginRendering`); tracked image sync skips exact no-op transitions, and requested screenshots add an in-frame swapchain-to-buffer copy after those passes with explicit sync transitions. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.Sync.cpp` |

## Reading `RenderStats`

`cpuFrameMs` is the renderer submit window: after swapchain acquire and screenshot-setup through queue-submit bookkeeping. It excludes present pacing, resize handling, and screenshot readback waits.

CPU bucket fields are mutually exclusive:

- `cpuSceneBuildMs` — demo scene-list production.
- `cpuPrepareMs` — visibility planning, mesh lookup, culling, capacity growth, and overlay stats refresh.
- `cpuCommandRecordMs` — instance/indirect materialization, stat derivation, ImGui draw-data encoding, and Vulkan command recording.
- `cpuQueueSubmitMs` — queue submit setup and submission bookkeeping.

GPU timing fields are valid only when `gpuTimestampsValid` is true. When the prepass is disabled by `Auto` or `--no-depth-prepass`, `gpuDepthPrepassMs` is reported as zero and HDR timing includes the depth-writing scene pass.

Draw stats intentionally exclude ImGui draw lists. Scene draw counts include scene submissions plus the fullscreen tonemap draw.

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

- GPU occlusion/culling only after scene-scale telemetry justifies the extra buffers, barriers, and latency tradeoffs.
- Resource residency detail and allocation statistics by resource class.
- GPU-native compressed texture formats and streaming upload queues.
- Frame graph ownership of barriers, transient resources, and pass dependencies.
- Real descriptor indexing/bindless layouts when the resource model needs them.
- Submission benchmarks across larger mesh/material diversity before adding threaded command recording or GPU-driven submission.

## External references used for current choices

- Khronos wait-idle guidance: <https://docs.vulkan.org/samples/latest/samples/performance/wait_idle/README.html>
- Khronos command-buffer recycling guidance: <https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html>
- Khronos multi-draw indirect sample: <https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/multi_draw_indirect/README.adoc>
- NVIDIA Vulkan dos and don'ts: <https://developer.nvidia.com/blog/vulkan-dos-donts/>
- Vulkan present modes: <https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html>
- Vulkan feature bits: <https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html>
- Vulkan pipeline cache guide: <https://docs.vulkan.org/guide/latest/pipeline_cache.html>
- Vulkan synchronization examples: <https://docs.vulkan.org/guide/latest/synchronization_examples.html>
