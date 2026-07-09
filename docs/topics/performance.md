# Performance model

VolkEngine's current renderer is optimized for explicit, measurable work. The rule is simple: keep hot paths allocation-free when practical, make synchronization visible, and expose enough counters to know which path ran.

## Hot-path rules

- Do not allocate per frame unless a device limit or visible-count growth requires it.
- Prefer persistent mapping for tiny CPU-to-GPU frame data.
- Keep staging uploads outside the normal frame loop.
- Submit one primary frame command buffer and one graphics queue submit per frame.
- Reset per-frame command pools after their fences signal.
- Avoid `vkDeviceWaitIdle` in normal rendering.
- Treat optional diagnostics (`--no-imgui`, `--no-gpu-timestamps`) as benchmark controls, not error paths.

## Implemented performance features

| Area | Current behavior |
| --- | --- |
| Frames in flight | Two frame slots decouple CPU prep from GPU completion without unbounded latency. |
| Presentation | `--no-vsync` prefers immediate mode for lowest present-queue latency when available. |
| Geometry | Generated meshes are packed into shared device-local vertex/index buffers. |
| Scene instances | Per-frame mapped storage buffer starts at 2048 visible instances and grows after the frame fence. |
| Visibility | CPU frustum culling, mesh-bucket counts, material-grid tile acceleration, temporal static-grid visibility cache. |
| Submission | Multi-draw indirect when required Vulkan feature bits are enabled; direct indexed-instanced fallback otherwise. |
| Uploads | Mesh staging uses transfer queue plus semaphore when separate, or same-queue barriers when not. |
| Command buffers | One primary command buffer per frame; per-frame command pools reset as a unit. |
| Timestamps | Fixed timestamp range per frame slot; `gpuTimestampsValid` distinguishes real timings from pending/unavailable data. |
| Pipeline cache | Header-validated cache load/save with temp-file publish to avoid corrupting prior cache data. |
| Resource accounting | Fixed-capacity registry reports live renderer-owned/imported estimated bytes without owning memory. |

## Reading `RenderStats`

`cpuFrameMs` is the renderer submit window: after swapchain acquire/screenshot setup through queue-submit bookkeeping. It excludes present pacing, resize handling, and screenshot readback waits.

CPU bucket fields are mutually exclusive:

- `cpuSceneBuildMs` — demo scene-list production.
- `cpuPrepareMs` — visibility planning, mesh lookup, culling, capacity growth, overlay stats refresh.
- `cpuCommandRecordMs` — instance/indirect materialization, stat derivation, ImGui draw-data encoding, Vulkan command recording.
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
