# Performance model

VolkEngine's Vulkan renderer keeps public API in `VulkanRenderer.hpp` and implements measured hot paths across private `VulkanRenderer::Impl` split files. The exhaustive split map lives in [Renderer pipeline](renderer-pipeline.md#source-map--ownership-current-source-split); this page names only performance-sensitive owners.

## Performance-sensitive owners

- `engine/renderer/vulkan/VulkanRenderer.Frame.cpp`: `draw` orchestration, swapchain acquire/present, exception-safe command-buffer submission, and screenshot-in-frame integration.
- `engine/renderer/vulkan/VulkanRenderer.FrameResources.cpp`: per-frame command pools/buffers, mapped uniform/instance/indirect buffers, fences/semaphores, timestamp query pool, and frame graph compilation state.
- `engine/renderer/vulkan/VulkanRenderer.Visibility.cpp`: `SceneVisibilityPlan`, frustum culling, mesh-bucket accounting, material-grid tile acceleration, and temporal cache replay.
- `engine/renderer/vulkan/VulkanRenderer.Uploads.cpp`: one-shot upload command submission, upload queue tracking, completion fences, and queue-semaphore coordination.
- `engine/renderer/vulkan/VulkanRenderer.Sync.cpp`: image layout/state transitions, barrier construction, and per-frame sync snapshots for recovery.
- `engine/renderer/vulkan/VulkanRenderer.Pipelines.cpp`: shader module validation/load, pipeline layouts/pipelines, pipeline cache validation/publish, and shader hot-reload rebuild/retire rules.
- `engine/renderer/vulkan/VulkanRenderer.Resources.cpp`: long-lived texture/image/buffer resources, samplers, descriptor layouts/pools/sets, and resource registry accounting.
- `engine/renderer/vulkan/VulkanRenderer.Meshes.cpp`: generated cube/sphere/plane geometry and shared device-local vertex/index uploads.
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
| Geometry | Generated meshes are packed into shared device-local vertex/index buffers. | `VulkanRenderer.Meshes.cpp` |
| Scene instances | Per-frame mapped storage buffer starts at 2048 visible instances and grows after the frame fence. | `VulkanRenderer.FrameResources.cpp`, `VulkanRenderer.Frame.cpp` |
| Visibility | CPU frustum culling, mesh-bucket counts, material-grid tile acceleration, temporal static-grid visibility cache. | `VulkanRenderer.Visibility.cpp` |
| Submission | Multi-draw indirect when required Vulkan feature bits are enabled; direct indexed-instanced fallback otherwise. | `VulkanRenderer.Frame.cpp` |
| Uploads | Same-queue uploads use in-command transfer barriers (`VkBufferMemoryBarrier2`) from mesh upload staging; separate transfer queue uploads inject per-upload signal semaphores and are consumed by frame submit waits. | `VulkanRenderer.Uploads.cpp`, `VulkanRenderer.Meshes.cpp` |
| Command buffers | One primary command buffer per frame; per-frame command pools reset as a unit. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.FrameResources.cpp` |
| Timestamps | Fixed timestamp range per frame slot; `gpuTimestampsValid` distinguishes real timings from pending/unavailable data. | `VulkanRenderer.FrameResources.cpp` |
| Pipeline cache | Header/device validated cache load/save at `${binaryDir}/cache/pipeline_cache.bin` with temp-file publish and post-readback validation. | `VulkanRenderer.Pipelines.cpp` |
| Resource accounting | Fixed-capacity registry reports live renderer-owned/imported estimated bytes without owning memory. | `VulkanRenderer.Resources.cpp` |
| Diagnostics | Optional overlay is controlled by `--no-imgui` and runs only when `debugOverlay` is enabled; it refreshes stats snapshots at a fixed interval. | `VulkanRenderer.ImGui.cpp` |
| Dynamic rendering | Depth pass (optional), HDR scene pass, and tonemap pass are rendered with dynamic rendering (`vkCmdBeginRendering`); requested screenshots add an in-frame swapchain-to-buffer copy after those passes with explicit sync transitions. | `VulkanRenderer.Frame.cpp`, `VulkanRenderer.Sync.cpp` |

## Reading `RenderStats`

`cpuFrameMs` is the renderer submit window: after swapchain acquire and screenshot-setup through queue-submit bookkeeping. It excludes present pacing, resize handling, and screenshot readback waits.

CPU bucket fields are mutually exclusive:

- `cpuSceneBuildMs` — demo scene-list production.
- `cpuPrepareMs` — visibility planning, mesh lookup, culling, capacity growth, and overlay stats refresh.
- `cpuCommandRecordMs` — instance/indirect materialization, stat derivation, ImGui draw-data encoding, and Vulkan command recording.
- `cpuQueueSubmitMs` — queue submit setup and submission bookkeeping.

GPU timing fields are valid only when `gpuTimestampsValid` is true. With `--no-depth-prepass`, `gpuDepthPrepassMs` is reported as zero and HDR timing includes the depth-writing scene pass.

Draw stats intentionally exclude ImGui draw lists. Scene draw counts include scene submissions plus the fullscreen tonemap draw.

## Benchmark switches

Use sandbox flags to isolate costs:

- `--no-vsync` — minimize present pacing.
- `--no-imgui` — remove overlay setup, diagnostics refresh, and draw data.
- `--no-gpu-timestamps` — remove query pool and timestamp writes.
- `--no-indirect-draws` — force direct draw fallback.
- `--depth-prepass` / `--no-depth-prepass` — compare prepass cost against depth-writing HDR path.
- `--grid-rows N --grid-columns N` — scale scene-list size.
- `--grid-tile-rows N --grid-tile-columns N` — measure material-grid tile granularity.
- `--frames N --resize-smoke` — repeatable automated smoke.

## Known next work

- GPU occlusion/culling only after scene-scale telemetry justifies the extra buffers, barriers, and latency tradeoffs.
- Resource residency detail and allocation statistics by resource class.
- Production image formats and streaming upload queues.
- Frame graph ownership of barriers, transient resources, and pass dependencies.
- Real descriptor indexing/bindless layouts when the resource model needs them.
- Submission benchmarks across larger mesh/material diversity before adding threaded command recording or GPU-driven submission.
- Adaptive depth-prepass selection with benchmark windows and hysteresis, not per-frame thrash.

## External references used for current choices

- Khronos wait-idle guidance: <https://docs.vulkan.org/samples/latest/samples/performance/wait_idle/README.html>
- Khronos command-buffer recycling guidance: <https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html>
- Khronos multi-draw indirect sample: <https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/multi_draw_indirect/README.adoc>
- NVIDIA Vulkan dos and don'ts: <https://developer.nvidia.com/blog/vulkan-dos-donts/>
- Vulkan present modes: <https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentModeKHR.html>
- Vulkan feature bits: <https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html>
- Vulkan pipeline cache guide: <https://docs.vulkan.org/guide/latest/pipeline_cache.html>
- Vulkan synchronization examples: <https://docs.vulkan.org/guide/latest/synchronization_examples.html>
