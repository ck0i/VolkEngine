# Performance

Performance work must name the workload, hardware, driver, build, settings, and before/after distribution. A lower average without tail latency, image quality, and submitted-work context is not sufficient.

## Hot-path constraints

- No steady-state frame allocation. Frame-slot buffers may grow only after their fence signals.
- Keep small CPU-to-GPU data persistently mapped.
- Stage asset uploads outside normal frame work.
- Record one primary frame command buffer and submit once to the graphics queue.
- Reset command pools by frame slot after fence completion.
- Avoid `vkDeviceWaitIdle` outside recovery or teardown.
- Treat ImGui, timestamps, shadows, Hi-Z, indirect draws, and depth-prepass modes as independent benchmark variables.

## Current design

| Area | Contract |
| --- | --- |
| Frames | Two slots bound latency and resource reuse. |
| Jobs | Fixed capacities, local LIFO work, FIFO stealing, dependencies, cooperative waits, and explicit handle release. |
| Simulation | Only independent read-only systems share a phase; mutation remains serial. |
| Assets | IO and import jobs publish complete candidates at a frame boundary; failure retains active assets. |
| Residency | One byte budget covers typed artifacts; requested dependencies are pinned and unpinned LRU entries are evicted. |
| Scene data | Reusable extraction and frame-slot arrays avoid rebuilding unchanged records; covered material grids reuse revision-keyed GPU records and capacity counts, and material features are normalized during GPU instance materialization instead of reconstructed per fragment. |
| Visibility | Capability-gated compute performs culling, LOD, Hi-Z rejection, compaction, counters, and command generation. |
| Submission | The default GPU path emits one indirect command per mesh; direct submission remains the fallback. |
| Lighting | Fixed tile and shadow-atlas partitions bound pressure; workgroups share projected light tile ranges, caster culling and range checks avoid unnecessary work and reduce attenuation bounds to one-sided clamps, environment lobes share probe weights, and uploads precompute inverse range-squared, cone-width, integer probe counts, local/directional/environment radiance, and environment rotation outside fragment loops. |
| Uploads | Meshes and textures are packed into shared staging submissions; same-queue barriers avoid unnecessary semaphore chains. |
| Frame graph | Cached variants derive pass order, hazards, lifetimes, and compatible transient slots. |
| Diagnostics | Fixed timestamp ranges and bounded job/streaming traces expose cost without unbounded telemetry growth. |

## Reading telemetry

`cpuFrameMs` covers renderer preparation through queue-submit bookkeeping. It excludes present pacing, resize handling, and screenshot readback waits. Its buckets are:

- `cpuSceneBuildMs` — render-list production;
- `cpuPrepareMs` — visibility, capacity, acquire, screenshot, and diagnostics setup;
- `cpuCommandRecordMs` — instance data, statistics, UI encoding, and command recording;
- `cpuQueueSubmitMs` — submission setup and API call.

GPU fields are valid only when `gpuTimestampsValid` is true. Pass timings cover light assignment, culling, shadows, depth, HDR, depth-pyramid, and final output. Run summaries report post-warmup distributions for each active pass; disabled pass distributions have no samples, while their scalar `RenderStats` timing remains zero. When the prepass is off, HDR includes depth writes.

`sceneTriangleCount` describes visible geometry before pass multiplication. `triangleCount` describes submitted work, including repeated scene passes and fullscreen output. ImGui draw lists are excluded.

## Benchmark controls

```text
--no-vsync
--no-imgui
--no-gpu-timestamps
--no-indirect-draws
--hiz-occlusion / --no-hiz-occlusion
--auto-depth-prepass / --depth-prepass / --no-depth-prepass
--shadows / --no-shadows
--grid-rows N --grid-columns N
--frames N --resize-smoke
```

Use `VolkEngineJobSystemBenchmark` for scheduler scaling and `VolkEnginePartitionBenchmark --benchmark-gate --no-vsync` for the streamed landscape gate.

## Near-term measurements

- M3 4K fidelity/performance matrix on the target GPU classes.
- Cross-driver image and performance policy.
- Resource-class residency and allocation detail.
- GPU-native compressed upload and streaming.
- Flat versus hierarchical cluster traversal before adopting a hierarchy.
- Physical transient-memory aliasing only where graph overlap produces a measurable saving.

Useful references: [Vulkan wait-idle guidance](https://docs.vulkan.org/samples/latest/samples/performance/wait_idle/README.html), [command-buffer usage](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html), [multi-draw indirect](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/multi_draw_indirect/README.adoc), and [synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).
