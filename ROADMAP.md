# VolkEngine Roadmap

VolkEngine is being built as a general-purpose, production-oriented AAA game engine for small expert teams and engine programmers. Its defining goals are high graphical fidelity per millisecond, massive and densely populated worlds, compact modern architecture, and an engine foundation that serious teams can inspect, extend, and own.

This roadmap describes capability milestones, not calendar promises or a public task board. The order matters: later milestones depend on the ownership, data, tooling, and measurement contracts established by earlier ones. Exact designs may change when prototypes or measurements disprove an assumption.

## Product direction

VolkEngine should eventually let a small expert team author, profile, package, and ship a high-fidelity game without turning the project into an unmaintainable collection of renderer demos and disconnected subsystems.

The intended shape is:

- A C++ engine and runtime with C# as the primary supported gameplay scripting language through modern .NET hosting.
- A familiar entity-and-component authoring model rather than requiring game authors to work directly against a low-level data-oriented ECS API.
- A Vulkan 1.3 renderer for Windows and Linux, organized around backend-neutral resource and pass contracts where that portability has a concrete use.
- A hybrid rasterization and hardware ray-tracing renderer. Raster remains the scalable foundation; ray tracing augments reflections, shadows, and global illumination when hardware and frame budgets permit it.
- A Forward+ lighting path, GPU-driven visibility, virtualized geometry in the longer term, and world streaming built for both geographic scale and massive object density.
- A full editor as a core product requirement. Dear ImGui is an interim implementation tool, not the intended final authoring UI.
- An extensible ecosystem for native plugins, managed packages, importers, content, and community UI implementations. Formal package registry and marketplace work comes only after local package contracts are proven.

The first proving ground is a reproducible cinematic benchmark built around a large natural landscape. It should stress terrain, foliage, world traversal, object density, streaming, atmosphere, lighting, and materials before VolkEngine claims broad production readiness.

## Decision rules

When priorities conflict, VolkEngine will prefer:

1. **Correctness, explicit ownership, and recoverable failure handling** over cleverness.
2. **Compact, maintainable code** over duplicate conventions, speculative layers, or feature-count growth.
3. **Data-oriented scalability** over APIs that hide allocation, synchronization, batching, or residency costs.
4. **Measured performance** over theoretical optimization or unsupported fidelity claims.
5. **Complete production workflows** over isolated subsystem demonstrations.
6. **Depth over breadth**: a smaller set of dependable capabilities is more valuable than many shallow checkbox features.
7. **Clean pre-1.0 evolution**: APIs and formats may break when the architecture improves; in-tree users must be migrated without compatibility shims.

Dependencies should be minimized. VolkEngine should own its differentiating renderer, runtime, asset, editor, and extension contracts. A third-party integration is justified when rebuilding the subsystem would be unrealistic and the licensing, maintenance, performance, binary-size, and failure-mode costs are understood.

## Current state

**Status: implemented foundation**

As of July 2026, the repository contains a C++23 static engine library, a GLFW/Vulkan sandbox, and a separable interim ImGui editor executable.

### Engine and scene foundation

Implemented:

- Generational sparse-set ECS storage with guarded queries and deferred structural commands.
- Deterministic compiled simulation scheduling with explicit dependency-safe read-only parallel phases and serial mutable boundaries.
- Fixed-capacity transactional typed events and timers.
- Fixed-step simulation, accumulated input, camera, time, and file helpers.
- Fixed-capacity dependency-aware work stealing with cooperative waits, cancellation/failure propagation, background asset IO/cooking, and bounded profiling.
- Persistent UUID entity identity, validated scene hierarchy, fixed-step TRS interpolation, and deterministic CPU scene extraction.
- Bounded transactional VESN v2 world snapshots, stable reflected component metadata, versioned authoring documents, and deterministic cooked-world instantiation.
- Bounded unified runtime residency for texture, geometry, world-cell,
  animation, and audio artifacts, plus deterministic hierarchical world
  partition selection, retained-frontier publication, and local-origin rebasing.

Current limits:

- No general thread-safe world mutation model or headless game application lifecycle.
- Reflection currently covers the built-in authoring components rather than arbitrary gameplay/ECS pools, and VESN remains an explicit low-level scene subset.
- No prefab/override model, asset GUID database, dirty extraction, gameplay serialization, or project-level editor metadata; streamed partition publication still rebuilds a complete CPU `World`.
- No C# host, Lua integration, production input mapping, runtime UI contract, physics, audio, animation stack, navigation, networking, or replay system.

### Renderer foundation

Implemented:

- Vulkan 1.3 dynamic rendering with Vulkan Memory Allocator and two frames in flight.
- HDR rendering, ACES tone mapping, reverse-Z, and an adaptive depth prepass.
- Forward+ tiled local-light assignment for bounded point and spot lights, one directional light, deterministic light-list overflow, and one compute dispatch per frame.
- A fixed 2048² depth atlas provides three directional cascades plus deterministic local spot-shadow slots, slope-scaled receiver bias, stable cascade fitting, guarded projected-depth sampling, and explicit atlas-pressure telemetry.
- HDR image-based diffuse/specular lighting uses a mipmapped linear environment map, bounded spherical reflection probes, exposure compensation, and ACES output.
- The packed PBR path covers standard, masked, clear-coat, foliage, skin, hair, cloth, emissive, landscape, and water classes without per-material descriptor growth; analytic atmosphere, procedural terrain response, and GPU foliage wind share the Forward+ path.
- CPU fallback visibility plus capability-gated GPU cluster frustum/LOD/temporal-occlusion culling, visible-instance compaction, and generated multi-draw indirect submission.
- Upload synchronization, pipeline caching, shader hot reload, per-pass GPU timestamps, renderer statistics, ImGui diagnostics, swapchain recovery, and PPM screenshots.
- Executable frame-graph variants drive Forward+ assignment, cull, shadow atlas, depth, HDR, temporal depth-pyramid, tone-map, and screenshot work with explicit synchronization/lifetime contracts and machine-readable diagnostics.

Current limits:

- The renderer is single-view and forward-oriented; local-light assignment is bounded rather than clustered in depth, reflection probes share the current environment texture, and transparency, temporal antialiasing, upscaling, post effects, hardware ray tracing, production global illumination, and robust device-loss policy remain absent.
- GPU submission currently uses a flat bounded-cluster list; it does not yet traverse a cluster hierarchy or provide fine-grained mesh/cluster LOD beyond the existing sphere mesh tiers.
- The temporal Hi-Z implementation is correctness-proven on one integrated Vulkan driver, but representative cross-driver crossover gates are not yet established.
- The frame graph does not schedule multiple queues or physically alias Vulkan memory yet; current depth/HDR logical transients intentionally use separate allocation classes and are realized at swapchain scope.
- There is no virtual texture pipeline, GPU-page streaming scene representation, hardware ray tracing, production global illumination, cloud/weather/volumetric stack, or terrain-specific GPU page representation.

### Assets, tools, and delivery

Implemented:

- Stable 128-bit asset identity, transactional dependency records, SHA-256 derived-data keys, versioned atomic artifacts, and generational runtime handles.
- glTF 2.0 import for mesh primitives, scene hierarchy, metallic-roughness materials, texture roles/color spaces, bounds, and validated animation clip/channel metadata, routed through a deterministic extension registry.
- Incremental reference-asset cooking and transactional reload; artifact-content keys avoid rebuilding byte-identical outputs and propagate only real dependency changes.
- Versioned texture artifacts validate and preserve decoded RGBA8, linear RGBA32F HDR, and non-supercompressed KTX2 BC1/BC3/BC7 payloads with explicit role, color-space, dimensions, and mip metadata.
- OBJ/procedural geometry and direct stb_image-backed texture overrides remain available alongside the authored glTF path.
- Stable generated/explicit/external reflection manifests, transactional authoring commands and migration, canonical authoring/cooked-world formats, and a separable hierarchy/inspector/viewport/profiling editor shell.
- Unified asynchronous runtime artifact residency, canonical partition manifests
  and cell artifacts, transactional coarse/fine frontier publication, bounded
  per-frame streaming traces, and a live deterministic Vulkan traversal gate.
- Deterministic bounded landscape fields, brushes, collision/query samples, skirted hierarchical terrain patches, low-poly vegetation meshes, biome-driven foliage scatter, and water-patch cooking feed the same cooked-world/partition/runtime asset path.
- Twenty-seven CPU test executables plus sandbox, editor, and partition-benchmark help tests.
- Documentation for the public API, architecture, renderer pipeline, performance model, shaders, and assets.

Current limits:

- No FBX importer, animation sample-data artifact or runtime playback, platform packaging/cooking, material database, BasisLZ/Zstd texture transcoder, runtime HDR/BC texture upload, or GPU-native texture/geometry streaming publication path.
- The interim editor has no asset browser, prefab/override workflow, isolated play mode, project creation, packaging, installed SDK, plugin manifest, or package registry.
- A self-hosted live-Vulkan workflow is defined, but coverage still depends on controlled runner availability; cross-driver visual/performance policy and a maintained external-style sample project remain incomplete.

### Verified baseline

Current local baseline (11 July 2026):
- `ctest --preset linux-debug` passed all 31 registered tests; the editor-free `linux-runtime` preset passed all 29 of its registered tests and its generated build contains no `engine/editor` source or editor target.
- A 180-frame Vulkan smoke on Intel RPL-S graphics forced the depth prepass, enabled Khronos validation plus synchronization validation as required, resized 1280×720 → 1024×640 → 1280×720, recompiled graph variants three times, executed depth/HDR/tone-map/readback work, wrote a screenshot and schema-v2 summary, and passed the `resize-recompile-v1` regression gate.
- A separate validated 24-frame fault-injection smoke recovered a post-acquire failure by restoring tracked state, replacing the acquire semaphore, recreating the swapchain/graph resources, writing a screenshot, and exiting cleanly.
- A 24-frame authored-scene smoke resolved seven database records through the DDC, uploaded the cooked albedo/normal/ORM artifacts, rendered the three-node/two-primitive hierarchy through multi-draw indirect submission, wrote `s2-authored-scene.ppm` plus a schema-v2 run summary, and exited cleanly.
- A validated 24-frame dense-scene smoke at the odd extent 1281×721 executed GPU cluster culling and temporal Hi-Z, rejected 6,350 of 22,921 tested cluster instances in its captured completed frame, rendered a screenshot, and emitted explicit cull/Hi-Z timings and descriptor pressure without validation errors. A matching no-occlusion GPU capture was pixel-identical.
- On the same Intel RPL-S driver, a final paired 110-sample schema-v3 release run at 65,544 instances established the first controlled crossover: default capability-gated subgroup-reserved mesh-command generation plus temporal Hi-Z measured 23.935 ms median GPU frame time versus 27.629 ms for CPU grid culling/direct submission, a 13.4% reduction. The GPU path submitted 15,673,889 triangles instead of 23,992,353 and three draw calls instead of 15. Its CPU-frame median remained higher (3.002 ms versus 1.726 ms) and GPU p95 remained worse (30.526 ms versus 28.128 ms), so this is a bounded crossover result rather than a universal policy claim.
- The S4 gate passed all 24 registered debug tests and a strict 120-frame Vulkan run on Intel RPL-S graphics. The run required Khronos and synchronization validation, exercised 32 local lights, seven active shadow views, two reflection probes, the seven non-masked material classes, the HDR environment, GPU visibility validation, depth-prepass rendering, two resize/recompile events, and screenshot readback without validation errors; focused CPU contracts and shader compilation cover the eighth, masked class. Schema-v4 telemetry reported zero tile/atlas overflow, 0.045 ms light assignment, 2.432 ms shadow rendering, a 14.734 ms median GPU frame, and a 0.712 ms median CPU render-submit frame across 90 post-warmup samples. A separate strict `--no-shadows` run reported zero shadow views and an unavailable shadow timing with reason `pass disabled`.

- S5's deterministic job benchmark executed 128 independent one-million-iteration integer jobs over three rounds with checksum equality. The best exact 1-worker sample was 502.060 ms and the best exact 8-worker sample was 61.353 ms: an 8.183× speedup with nine steals.
- A 1,400-frame Vulkan asset-reload smoke on Intel RPL-S graphics changed and restored a live authored texture, published both complete candidates without stopping rendering, wrote a visually inspected 1280×720 screenshot and schema-v5 summary, and exited cleanly. Its 25 jobs all succeeded; telemetry recorded six IO jobs, nineteen Asset jobs, twenty-three steals, a queue high-water mark of three, and 53.903 ms aggregate worker time. Validation was requested but unavailable on the current machine, so this is functional publication evidence rather than new validation-layer evidence.
- A separate 1,400-frame corrupt-source smoke rejected two invalid-image candidates, retained the active CPU/GPU bundle, continued rendering, emitted a schema-v5 summary, and exited cleanly. The final profile reported twenty-one succeeded and four failed jobs with no leaked active work.
- A 90-frame `VolkEngineEditor --editor-smoke` run imported the reference glTF, created/reparented/edited an entity, exercised undo/redo, atomically saved and reopened a 584-byte authoring document, cooked and loaded a 404-byte runtime world, rendered the two authored primitives, captured the hierarchy/inspector/viewport/profiling shell at 1280×720, emitted a schema-v5 summary, reported zero failed jobs, and exited cleanly. Validation was requested but unavailable on the current machine, so this is functional creator-workflow evidence rather than new validation-layer evidence.

- The M1 `partition-traversal-v1` gate completed 1,320 Vulkan frames and 1,260
  measured traversal samples across a deterministic 14 km out-and-back path.
  It streamed a canonical 21-cell hierarchical world through shared IO jobs,
  rendered authored geometry from active cells, shifted origin eleven times,
  published fifteen complete frontiers, retained coverage for six transition
  frames, evicted under a 46,116-byte budget, recorded zero main-thread IO,
  coverage gaps, partial failures, or failed jobs, and emitted a schema-v6
  summary plus an inspected 1280×720 screenshot. Validation was requested but
  unavailable locally; the controlled GPU workflow now runs this same gate with
  validation required.
- The M2 `landscape-traversal-v1` gate completed 1,320 Vulkan frames and 1,260 measured traversal samples over a deterministic 28 km out-and-back route. Its canonical 21-cell hierarchy streamed 21 terrain patches across three LOD tiers, 667 biome-selected foliage instances across three reusable meshes, and 16 water patches while analytic atmosphere and GPU vertex wind remained active. Schema-v7 evidence recorded a stable SHA-256 content identity, fifteen publications, eleven origin shifts, four evictions, six backpressure events, zero coverage gaps, partial loads, main-thread IO, IO/missing-dependency/OOM failures, and CPU/GPU p95 frame times of 1.718/14.528 ms under explicit 16.667 ms budgets. The run emitted an inspected 1280×720 screenshot; validation was requested but unavailable locally.
This baseline establishes S1's executable frame-graph/synchronization contract, S2's authored-asset workflow, S3's validated GPU-generated visibility and measured crossover foundation, S4's Forward+/shadow/HDR material baseline, S5's bounded parallel execution plus transactional background asset-publication path, S6's reflected authoring-to-cooked-runtime creator loop, M1's bounded asynchronous residency/partition traversal contract, and M2's reproducible streamed natural-landscape benchmark on one local machine. It is evidence of a working renderer and engine foundation, not production readiness or cross-hardware correctness.

## Milestone status

| Status | Meaning |
| --- | --- |
| **Current** | Implemented in the repository and covered by the present baseline. |
| **Next** | The active dependency chain. Design approval is still required before implementation work is claimed. |
| **Later** | Directionally approved, but blocked by earlier contracts or evidence. |
| **Research** | Valuable but design-sensitive work that must earn production integration through measurement. |

Roadmap entries are not claimable issues. See [CONTRIBUTING.md](CONTRIBUTING.md) before proposing work.

# Short term: establish the production dependency chain

“Short term” means the next dependency-ordered capability chain, not a date estimate. Near-term effort should remain approximately renderer-heavy: roughly 70% renderer/infrastructure and 30% asset/editor foundation. The goal is to make the existing renderer extensible and measurable while producing the first visible creator workflow: an authored scene imported through a real asset pipeline.

## S1. Executable frame graph and explicit resource ownership — **Current**

The former metadata planner is now the renderer's execution and resource-lifetime backbone.

Scope:

- Define pass setup and execution contracts with explicit read/write usage, attachment behavior, and synchronization intent.
- Move image/buffer ownership, lifetime tracking, and deferred destruction behind measured renderer contracts.
- Add transient-resource allocation and aliasing only after lifetime correctness is observable.
- Preserve a Vulkan-first implementation while keeping pass, resource, and shader-facing concepts free of gratuitous Vulkan leakage where a future backend would need a different representation.
- Expose graph structure, barriers, lifetimes, allocation, and per-pass timing through built-in diagnostics.
- Establish device-loss and swapchain-recovery ownership boundaries rather than scattering recovery special cases across passes.

Exit criteria:

- The sandbox’s depth, opaque/HDR, and tone-map work executes through the graph rather than a parallel manual path.
- Resource creation, use, transition, retirement, and failure behavior have deterministic CPU contract tests.
- A live Vulkan smoke run covers resize, swapchain recreation, graph recompilation, and validation-layer execution on a configured GPU runner.
- Pass timings and transient memory usage are visible and machine-readable.

Current evidence:

- Depth-prepass-on/off and screenshot-on/off variants dispatch depth, HDR, tone-map/ImGui, and readback callbacks exclusively through `FrameGraph::execute`.
- CPU contracts cover compilation, attachment/usage validation, topology, barriers, lifetimes, transient-slot aliasing, lifecycle order, reverse-order failure unwind, and Vulkan usage mapping.
- The controlled GPU workflow requires Khronos validation plus synchronization validation and exercises resize/recreation, injected post-acquire recovery, graph recompilation, screenshots, and machine-readable summaries.
- `RenderStats`, ImGui, and run-summary schema v2 expose pass/resource/barrier counts, transient requested/allocated bytes, physical slot count, compile/recompile state, and depth/HDR/tone-map GPU timings.

## S2. Asset identity, derived-data cache, and authored-scene import — **Current**

Build the minimum real content pipeline in parallel with S1. This is the first contributor-visible product milestone.

Scope:

- Introduce stable asset identifiers, source metadata, dependency records, importer versions, and content hashes.
- Build a transparent incremental derived-data cache. Source changes should rebuild only affected outputs; runtime code should consume versioned engine-native artifacts rather than ad hoc source files.
- Implement glTF 2.0 first for meshes, scene hierarchy, standard PBR materials, textures, and the subset of animation metadata needed by later milestones.
- Add robust image/HDR/compressed-texture ingestion and color-space metadata.
- Define importer extension contracts before adding broad format support.
- Add FBX only through a separately approved integration with its SDK, licensing, animation, material, and reproducibility costs documented.
- Replace fixed startup textures and OBJ-only assumptions in the sandbox with asset references resolved through the database/cache.

Exit criteria:

- A representative authored glTF scene imports, caches, reloads incrementally, and renders with correct hierarchy, transforms, material texture roles, color spaces, and bounds.
- Missing, corrupt, cyclic, stale, and incompatible assets fail with actionable diagnostics and without partially mutating the asset database.
- A second clean checkout reproduces the same cooked outputs from the same inputs and tool versions.
- Dependency and cache behavior has deterministic tests; the imported scene has a screenshot and live-render smoke path.

Current evidence:

- Stable IDs, transactional dependency records, importer/settings/source hashes, content-addressed versioned artifacts, runtime handles, and atomic database/cache publication are active in the sandbox path.
- The registered cgltf importer produces deterministic mesh, material, scene, animation-metadata, and texture artifacts; unknown/duplicate importer extensions fail before import.
- Texture contracts decode and validate common RGBA8 sources and Radiance HDR, preserve finite RGBA32F data, and validate bounded non-overlapping KTX2 BC1/BC3/BC7 mip payloads while rejecting unsupported formats and supercompression actionably.
- CPU contracts cover cold/warm/independent cooks, selective animation and decoded-texture invalidation, equivalent-source cache reuse, missing/corrupt/cyclic/stale/incompatible failures, failed-reload preservation, artifact round-trips, and DDC corruption.
- The maintained authored fixture contains a three-node hierarchy, two independently transformed mesh primitives, a metallic-roughness material with sRGB albedo plus linear normal/ORM roles, and validated animation metadata. Its live screenshot path consumes cooked artifacts rather than source image bytes.

## S3. GPU-driven geometry and bindless material foundation — **Current**

Build the prerequisites for dense worlds before attempting virtualized geometry.

Scope:

- Replace the fixed texture array with a real capability-gated bindless resource model and stable material/resource handles.
- Cook meshes into meshlets or equivalent bounded clusters with hierarchy and bounds metadata.
- Move instance/cluster frustum culling, LOD selection, and indirect-command generation to GPU compute.
- Add depth-pyramid occlusion after the generated draw path is correct and measurable.
- Keep explicit fallbacks only where the Vulkan capability baseline requires them; do not preserve duplicate rendering architectures without evidence.
- Report visible and rejected instances/clusters, generated work, bandwidth, descriptor pressure, and CPU submission cost.

Current implementation:

- Capability-gated bindless sampled-image descriptors use stable texture-table indices with a fixed-set fallback.
- Mesh upload cooks bounded clusters/ranges; compute performs instance and optional cluster frustum tests, sphere LOD selection, visible-instance compaction, and indexed indirect-command generation.
- Mesh-granularity indirect commands are the measured default. Cluster-granularity commands remain an explicit diagnostic/workload option; subgroup ballot reservation is selected when supported and a bounded atomic fallback preserves portability.
- Temporal occlusion reads the previous pyramid before current rendering, then conservatively reduces current reverse-Z depth into a half-resolution odd-extent-safe pyramid for the next submission.
- CPU contracts cover storage-image usage/synchronization and temporal read-before-write graph order. GPU visibility validation compares completed generated counts/commands with a CPU reference when occlusion is intentionally disabled.
- `RenderStats`, ImGui, and schema-v3 summaries expose descriptor pressure, cooked clusters, active culling-unit granularity and visible/tested/occluded counts, plus separate cull/depth/HDR/Hi-Z/final timings.

Exit criteria:

- A deterministic dense-scene benchmark renders many materials and instances without per-material descriptor rebinding or CPU draw enumeration.
- GPU culling and generated submission are correctness-tested against a bounded CPU reference on controlled scenes.
- Representative captures show no resource-lifetime, synchronization, descriptor, or visibility errors.
- Measurements demonstrate the scale at which the GPU-driven path outperforms the current CPU path; unsupported claims are not accepted.

Current evidence:

- The 65,544-instance controlled release pair above establishes the first measured median GPU-frame crossover while retaining explicit CPU and tail-latency counterevidence.
- Khronos validation plus synchronization validation passes the generated mesh-command path; CPU-reference validation covers command partitions, visible counts, sphere LOD counts, and submitted triangle accounting.
- Dense material-grid instances carry independently varied metallic/roughness parameters without per-material descriptor rebinding or CPU draw enumeration; generated submission reduces the captured scene to one indirect call per scene pass.

## S4. Forward+ lighting, shadows, and production PBR baseline — **Current**

Create the first visibly modern lighting path on top of S1–S3.

Scope:

- Add tiled or clustered light assignment for scalable directional, point, and spot lights.
- Implement production directional shadows, then local-light shadows, with explicit atlas, update, filtering, and culling policies.
- Add image-based lighting, environment maps, reflection probes, exposure, and a correct linear/HDR color workflow.
- Expand the fixed material model into an optimized PBR library for common opaque, masked, emissive, clear-coat, foliage, skin, hair, and cloth needs as justified by benchmark content.
- Provide controlled programmable shader extension points; do not commit to a node-material graph before the runtime permutation and reflection contracts are proven.
- Adopt Slang only after a measured toolchain prototype proves diagnostics, reflection, build integration, cache behavior, generated SPIR-V, and future portability superior to the current path.

Exit criteria:

- The imported reference scene supports many local lights, directional/local shadows, environment lighting, and multiple production material classes within published frame and memory budgets.
- Light-list overflow, shadow-atlas pressure, shader compilation failure, and unsupported material features are observable and deterministic.
- Visual regression scenes cover light boundaries, shadow stability, normal/roughness/metalness response, exposure, and color-space handling.

Implemented:

- A CPU-planned, GPU-consumed 16×16 Forward+ tile grid assigns up to 256 point/spot lights with 64 entries per tile and deterministic overflow accounting.
- Three practical-split directional cascades and packed local spot-shadow views render into a deterministic 2048² atlas. Frame-graph ordering, dynamic depth rendering, receiver bias, atlas sampling bounds, and shadow/no-shadow fallback are validation-clean.
- A generated linear HDR equirectangular map with radiance-preserving mips drives roughness-dependent specular and diffuse environment lighting. Four bounded spherical probes blend deterministic tint/intensity overrides without allocating per probe.
- Standard, masked, clear-coat, foliage, skin, hair, cloth, and emissive classes share one ABI and one Forward+ shader path. Masked depth/shadow variants preserve authored alpha cutoff; all class counts are observable.
- Renderer diagnostics publish local-light count, tile overflow, shadow views/capacity/overflow, reflection-probe count, material-class coverage, environment/exposure state, and independent light-assignment/shadow GPU intervals through ImGui, the terminal summary, and run-summary schema v4.

## S5. Job system, asynchronous IO, and profiling spine — **Current**

Parallelism is required for streaming and world scale, but it must not make ownership or determinism implicit.

Scope:

- Add a bounded work-stealing scheduler with dependencies, counters/fences, cancellation, and non-blocking wait support through fibers or coroutines where measurements justify it.
- Preserve explicit deterministic simulation phases while allowing safe systems and extraction work to run in parallel.
- Integrate file IO, decompression, asset import, cache work, and renderer upload preparation into one observable scheduling model.
- Define world mutation/command boundaries rather than making all ECS operations silently thread-safe.
- Add built-in CPU job timelines, worker utilization, queue depth, stall, cancellation, and async-IO diagnostics.

Exit criteria:

- Tests cover dependency ordering, cancellation, shutdown, worker exhaustion, nested waits, deterministic phases, and failure propagation.
- Asset import and background loading execute without blocking the main/render thread on normal paths.
- Profiling demonstrates useful parallel speedup on a modern eight-core desktop without regressions in deterministic simulation behavior.

Implemented baseline:

- `JobSystem` preallocates generational job slots, dependency edges, per-worker queues, and a bounded timeline; ready work uses local LIFO execution and cross-worker FIFO stealing. Cooperative waits, cancellation, failure propagation, clean drain/cancel shutdown, explicit terminal release, capacity errors, and aggregate/category metrics have focused contracts.
- `WorldSystemScheduler` preserves stable topological order, compiles explicit read-only systems into dependency-safe parallel phases, and places serial barriers around every mutable system. Phase failure joins submitted work, propagates the first exception, and rolls back deferred structural commands and scheduler-owned simulation resources.
- `ReferenceAssetCookTask` runs the parent cook asynchronously. Unique texture inputs dispatch IO source-read jobs followed by dependent Asset decode/import jobs; the source bytes are hashed and decoded once, and one-worker nested execution is covered.
- `Application` polls background reloads without waiting on normal frames. Changed candidates publish at a main-thread safe point through `VulkanRenderer::reloadReferenceAssets`; geometry, clusters, textures, descriptors, authored draws, and visibility caches cut over transactionally, while failure retains the old CPU/GPU bundle.
- Run-summary schema v6, shutdown logs, and the live title expose worker/job/category counts, active/running work, queue high-water mark, steals, and worker time. The deterministic exact 1-worker/8-worker benchmark and complete 30-test debug suite satisfy the local S5 gate.

## S6. Reflected authoring model and interim editor foundation — **Current**

Prevent the renderer-only trap by establishing a real creator workflow while APIs are still allowed to evolve.

Scope:

- Prototype annotated C++ code generation, explicit registration, and external schemas before selecting the reflection/binding source of truth.
- Generate stable type/property metadata, version information, serialization hooks, inspector metadata, and future C# bindings from the selected model.
- Replace the explicit VESN component subset with two forms: an authoring document and a deterministic optimized cooked-world representation.
- Build an interim ImGui editor shell with scene hierarchy, reflected inspector, viewport picking, transform gizmos, snapping, multi-selection, local command-based undo/redo, dirty state, save/load, and rendering/profiling views.
- Keep runtime/editor dependencies separable. ImGui implementation details must not become the permanent editor document or command model.
- Defer final prefab semantics until reflection, serialization, overrides, and real editor workflows expose the required composition rules.

Exit criteria:

- A user can import the reference scene, create and reparent entities, edit reflected components, undo/redo edits, save an authoring scene, cook it, reopen it, and render equivalent runtime content.
- Unknown/incompatible component data, migration failure, undo failure, and invalid hierarchy operations are handled transactionally.
- Editor-only code is absent from a headless/runtime build configuration.

Implemented baseline:

- Annotation-driven generation, explicit C++ registration, and the external line schema produce identical stable type/property binding manifests for reflected Transform v2 and Renderable v1 payloads. Known payloads have canonical little-endian hooks, bounded inspector metadata, and transactional v1-to-v2 transform migration.
- `AuthoringDocument` owns stable entities, hierarchy, known/opaque versioned component payloads, selection, content-fingerprint dirty state, and bounded command history. Create/delete/rename/reparent/component/property commands, multi-selection edits, preview gestures, undo/redo divergence, decode/migration failure, and hierarchy rejection preserve complete state transactionally.
- Canonical `VEAU` authoring files preserve unknown payloads; cooking rejects them explicitly. Deterministic `VECW` structure-of-arrays artifacts resolve authored mesh/material IDs through active generational renderer handles and construct a temporary runtime `World` before publication.
- `VolkEngineEditor` provides import/save/load/cook/runtime publication controls, hierarchy drag reparenting, reflected inspector edits, viewport picking, one-command snapped translation gizmos, multi-selection, dirty/undo/redo state, and CPU/GPU/job/frame-graph profiling through the renderer's non-owning overlay callback.
- `VolkEngineEditorCore` has no ImGui dependency; the UI/executable require both editor and ImGui options. The `linux-runtime` preset disables both and demonstrably emits no editor sources or targets.
- Focused authoring contracts cover generated/explicit/external manifest equivalence, known/unknown round trips, migration success/failure rollback, bounded history and undo divergence, hierarchy transactions, import/create/edit/save/reopen/cook/runtime equivalence, picking/snapping, and failed runtime resolution rollback. The complete debug suite passes all 27 tests.

## Cross-cutting short-term quality gates — **Next**

These gates grow with the milestones rather than waiting until the end:

- Self-hosted Windows and Linux build/test runners with controlled Vulkan hardware and drivers.
- Live Vulkan smoke tests with validation layers available and required.
- Deterministic benchmark camera paths with machine-readable CPU/GPU frame-time distributions, hitch counts, memory/residency, upload/streaming, scene-complexity, loading, and traversal metrics.
- Visual regression scenes with explicit tolerance and driver/hardware policy.
- Performance regression thresholds tied to named hardware and scene revisions.
- Built-in profiler views for frame graph, CPU jobs, GPU timings, memory, residency, streaming, and scene complexity.

# Medium term: prove a playable engine

The medium-term destination is a packaged playable vertical slice. Before that claim, VolkEngine must complete its cinematic natural-landscape benchmark and turn renderer, content, editor, runtime, and delivery systems into one workflow.

## M1. Unified streaming and world partition — **Current**

The S2 asset identity/DDC, S3 GPU-driven submission, and S5 bounded job/runtime
publication prerequisites now feed one deterministic runtime residency path.

- Use one dependency/residency scheduler for textures, geometry, world cells, animation, and audio.
- Add world partition cells, asynchronous dependency loading, residency budgets, eviction, origin management, and hierarchical LOD.
- Support both seamless geographic worlds and very high object density.
- Make loading priority, cancellation, backpressure, missing dependencies, and out-of-memory behavior explicit.
- Keep source authoring documents separate from optimized streamed runtime cells.

Implemented contract:

- `ResidencyManager` uses the application-owned `JobSystem` for bounded,
  dependency-expanded IO across texture, geometry, world-cell, animation, and
  audio classes. Stable priority, frame-scoped pinning, generation-safe
  cancellation, explicit failure states, LRU eviction, and per-class/aggregate
  pressure are deterministic and capacity guarded.
- Canonical `VEPW` manifests and per-cell `VECW` artifacts keep authoring source
  separate from runtime layout. Hierarchy/coverage validation, deterministic
  bounds-distance LOD, prefetch, complete-frontier assembly, cross-cell parent
  validation, and quantized local origins are executable contracts.
- Active cells remain pinned until a complete candidate instantiates into the
  live world and its matching partition revision commits. Failed, rejected, or
  stale candidates retain the old world; retries do not evict cells shared with
  that active frontier.
- `Application` exposes the shared scheduler, a once-per-render-frame
  transactional publication callback, renderer/job snapshots, and capped
  schema-v6 streaming evidence. The benchmark profiler displays live
  residency, queue, cell, publication, origin, coverage, and main-thread-IO
  state.

Exit gate: **Met.** `VolkEnginePartitionBenchmark --benchmark-gate` traverses a
14 km hierarchical world out and back with visible cooked cell geometry,
texture/geometry dependencies, asynchronous shared-job IO, a deliberately
constrained residency budget, eviction/backpressure, coarse-frontier retention,
and repeated origin shifts. The gate fails on main-thread IO, budget overflow,
coverage gaps, unrecoverable partial loads, missing dependencies, IO/OOM
failure, absent traversal, or absent visible geometry. Focused contracts cover
all five residency classes, dependency cycles, capacity/backpressure,
cancellation, stale completion, destruction with live IO, budget eviction,
pinned-frontier OOM, canonical manifests, hierarchy/coverage rejection,
corrupt-cell repair, shared-frontier retry, transactional publication, origin
rebasing, and zero-gap traversal.

## M2. Terrain, foliage, procedural generation, and atmosphere — **Current**

The cinematic natural-landscape benchmark now runs through the M1 streaming path and the S3/S4 GPU renderer.

Implemented contract:

- `LandscapeField` provides bounded deterministic height, normal, moisture, temperature, and biome queries plus revisioned radial height brushes. The same query contract supports cooking, collision/placement queries, and reproducible edits.
- `cookTerrainPatch` emits local-space, skirted indexed patches at three hierarchy-selected LOD tiers. Each partition cell owns a stable generated mesh ID; complete coarse/fine frontiers publish through existing `VECW` transactions and origin rebasing.
- Deterministic biome/slope/density scatter cooks grass, shrub, and tree instances into streamed cells. Reusable low-poly meshes keep geometry compact; existing GPU visibility, indirect submission, shadows, and shader-side wind handle runtime work without a second foliage renderer.
- Landscape and water are explicit material ABI classes. Procedural altitude/slope/moisture response, bounded water shading, analytic sky/atmosphere, and camera-relative foliage motion execute in the existing Forward+/HDR passes.
- Generated benchmark assets augment the cooked reference bundle transactionally before renderer construction. Untextured generated materials receive valid renderer fallback bindings without per-material descriptor growth.
- Schema-v7 summaries and the M2 ImGui overlay expose seed/content identity, LOD geometry, biome and species distributions, edit revision, visible terrain/foliage/water counts, traversal distance, budgets, residency pressure, and CPU/GPU distributions.

Exit gate: **Met.** `VolkEnginePartitionBenchmark --benchmark-gate` requires the full 1,200-frame measured traversal, visible terrain/foliage/water, at least 1,200 valid CPU and GPU timing samples, CPU/GPU p95 within the declared 16.667 ms budgets, zero coverage gaps/partial loads/main-thread IO/IO/OOM/missing-dependency failures, and resident bytes within budget. Focused contracts cover deterministic fields, seams, LOD metadata, brushes, water, all foliage density/species paths, bounded scatter failures, generated mesh bounds, material-class telemetry, and schema-v7 serialization.

## M3. Published fidelity and performance benchmark — **Next**

The initial target is a scene-budgeted native 3840×2160 at 60 FPS on an RTX 4070/RX 7800 XT-class GPU paired with a modern eight-core desktop CPU. A separate 120+ FPS scalability mode should publish its own resolution, content, and feature budgets rather than weakening the 4K claim invisibly.

The benchmark must disclose:

- Exact hardware, driver, build, scene revision, native resolution, and quality settings.
- Median, 95th-, and 99th-percentile CPU/GPU frame times and hitch counts.
- Host/device memory, residency, upload bandwidth, and streaming pressure.
- Loaded/visible instances, triangles or clusters, materials, lights, dispatches, and generated draws.
- Startup, load, traversal, and world-cell latency.
- Reference images and temporal-stability evidence.

A benchmark is a contract, not a marketing screenshot. If the scene misses its budget, quality, content, or architecture must change, or the published target must be revised explicitly.

## M4. C# gameplay and application framework — **Later**

Depends on reflection/schema and a separable runtime/editor application model.

- Embed modern .NET with generated, versioned native bindings.
- Make C# the primary supported language for gameplay loops, entity components, systems, events, input, assets, and scene interaction while retaining C++ for the engine backend and performance-critical native extensions.
- Support editor-time assembly reload with defined state-loss/state-migration behavior; shipping builds remain simpler and static.
- Add game/project lifecycle, headless execution, world transitions, settings, save data, error reporting, and debug/profiling integration.
- Consider Lua only after the scripting ABI is stable, initially for lightweight content logic or modding rather than as an equal duplicate API.

Exit gate: a separate sample game project can implement gameplay in C#, reload it in the editor, run it headlessly for tests, and package it without modifying engine source.

## M5. Production input and pluggable runtime UI — **Later**

- Add action maps, contexts, rebinding, keyboard/mouse/gamepad support, hot-plugging, chords, dead zones, sensitivity, hold/toggle accessibility options, and advanced-device extension points.
- Define stable UI services for rendering primitives, text/font resources, input/focus/navigation, clipping, localization, animation, accessibility hooks, and lifetime.
- Prove the extension model with one usable reference UI backend. Community packages may then provide alternative implementations, widgets, themes, layouts, fonts, and workflows.
- Start with local package manifests and compatibility metadata. Registry/marketplace discovery, trust, installation, and sandboxing come later.

Exit gate: the sample project ships a keyboard/mouse/gamepad-rebindable menu and HUD through the reference backend, and a second package can replace or extend that UI without renderer or input forks.

## M6. Minimum complete game stack — **Later**

These systems follow the cinematic showcase rather than blocking it:

- **Animation:** first deliver skeletal import, GPU skinning, clips, blending, state machines, events, and root motion. Retargeting, IK, motion warping, motion matching, control rigs, facial animation, cloth, hair, and cinematic sequencing remain later depth milestones.
- **Physics:** select or build the backend through a measured design review. Collision, queries, rigid bodies, triggers, and character control must integrate with the entity, job, transform, and serialization contracts. A pluggable seam is valuable only if it does not reduce correctness or performance.
- **Audio:** provide practical built-in spatial playback, streaming, attenuation, buses, mixing, and editor preview first; also preserve extension points for professional middleware integrations.
- **Gameplay support:** add the minimum navigation/AI, save/load, sequencing, debug, and runtime services required by the chosen vertical slice rather than attempting every genre system.

Exit gate: the maintained sample project demonstrates input, UI, C# gameplay, animation, collision/character interaction, spatial audio, save/load, editor iteration, headless tests, and packaged Windows/Linux execution.

## M7. Project, SDK, and packaging workflow — **Later**

- Support both an installed SDK for separate game repositories and a source workspace for engine contributors or deep engine forks.
- Create project manifests, reproducible dependency resolution, build profiles, asset cooking, packaging, logs/crash artifacts, and launch/debug workflows.
- Keep CMake as the native build foundation initially.
- Maintain one external-style sample project as a release gate, not only an in-tree sandbox.

Exit gate: clean Windows and Linux machines can obtain the supported toolchain, build the sample project, cook assets, run tests, package it, and execute the package using documented commands.

# Long term: production engine and research direction

These are directional destinations, not promises of dates or fixed designs.

## Virtualized geometry — **Research**

After bindless resources, meshlets, GPU culling, streaming, and executable resource ownership are proven, investigate a streamed cluster hierarchy with continuous or fine-grained LOD and an appropriate raster path. Integration requires measured wins over the production meshlet path in geometry density, traversal, memory, build time, and image stability. It must not become an unbounded research project that blocks shippable workflows.

## Hardware ray tracing and global illumination — **Research**

Rasterized Forward+ remains the baseline. Add capability-gated ray-traced reflections, shadows, and global illumination in that order only where each effect has a scalable raster/probe fallback and a published frame/memory budget. The final GI strategy remains intentionally undecided until lighting, material, temporal, streaming, and scene-scale requirements can be measured. A path-traced reference mode may be valuable for validating materials and lighting before it is viable as a real-time mode.

## AAA world and rendering depth — **Later**

- Virtual or otherwise aggressively streamed textures.
- Mature temporal antialiasing, dynamic quality controls, post processing, reflections, volumetrics, weather, water, transparency, particles, decals, and cinematic tools.
- Dense-city and open-world workflows, robust HLOD, procedural generation, collaboration-aware document boundaries, and source-control service extensions.
- Advanced character animation: retargeting, IK, motion warping, motion matching, control rig, facial systems, cloth, hair, and sequencing.
- Fidelity tiers that preserve explicit 4K60 and 120+ performance contracts rather than hiding cost behind defaults.

## Native editor product — **Later**

The interim ImGui editor should eventually migrate to a polished retained native UI when actual editor workflows justify the investment. The document model, command/undo model, reflection, viewport, asset services, and package contracts must survive that migration; ImGui widget structure must not define them.

A mature editor includes scene and prefab composition, asset import/cooking, material authoring, world partition, terrain/foliage/PCG tools, C# debugging, profiling, animation, audio, packaging, and extension management. Collaborative and source-control integrations follow proven local authoring rather than preceding it.

## Extension and package ecosystem — **Later**

Evolve local manifests into a versioned registry for native plugins, managed packages, importers, UI backends/themes, and content. Compatibility metadata, dependency resolution, provenance, trust, review, and sandbox boundaries must exist before calling it a marketplace. VolkEngine should ship strong reference implementations while allowing the community to replace or extend non-differentiating systems through stable contracts.

## Production 1.0 — **Later**

Version 1.0 means a production engine, not merely a showcase-ready renderer. Before that designation:

- A small expert team can author, profile, test, package, and ship a polished game through the supported editor/SDK workflow.
- Windows and Linux builds, packages, runtime tests, live Vulkan validation, visual regressions, and performance regressions run on controlled infrastructure.
- The external-style sample project exercises native engine code, C# gameplay, imported/cooked assets, editor workflows, runtime UI, core game systems, and packaging.
- Public native and managed APIs have generated references; architectural decisions and extension boundaries are documented.
- Failure handling, diagnostics, migration policy, performance budgets, ownership, and supported hardware are explicit.
- Major capabilities are maintained at production depth rather than existing only as demos.

Broader studio scale, proprietary console backends, and additional graphics APIs may follow demonstrated demand, access, and maintainership. Backend-neutral contracts should avoid gratuitous lock-in, but VolkEngine will not pay abstraction costs for hypothetical platforms.

# Explicit non-goals for now

The following should not distort the current architecture or sequencing:

- Mobile or web support.
- Networking, multiplayer replication, rollback, or large-scale online services.
- Console shipping promises without SDK access, hardware, and dedicated ownership.
- Building every commodity subsystem in-house when a carefully evaluated integration is the only realistic path.
- Backward-compatible public APIs or compatibility shims before 1.0.
- A permanent ImGui-based end-user editor.
- A package marketplace before local package and compatibility contracts work.
- Virtualized geometry or ray-tracing research that bypasses prerequisite infrastructure or measurable integration gates.
- Feature-count work that does not complete an authoring, runtime, debugging, or delivery workflow.

Selective deterministic simulation remains valuable for tests, replay/debugging, and designated gameplay systems. Cross-platform bitwise lockstep is not a current product requirement. Genre-specific systems should be introduced through real project needs; the core architecture remains genre-neutral.

# How contributors and AI agents should use this roadmap

This document communicates direction and dependencies. It does **not** grant ownership of a milestone or authorize an implementation.

Before non-trivial work begins:

1. Read [CONTRIBUTING.md](CONTRIBUTING.md) and the relevant architecture/API documentation.
2. Open a focused proposal identifying the problem, milestone dependency, scope, design, ownership/lifetime rules, performance impact, validation, and explicit non-goals.
3. Wait for maintainer design approval. Risky work may be asked to produce a bounded prototype and evidence before production integration.
4. Implement only the approved scope and provide the same correctness, runtime, visual, and performance evidence required from any other contributor.

AI-assisted contributions are held to the same engineering standard as human-written work. The contributor must disclose assistance, understand and own every changed line, explain the design, remove invented or placeholder code, and provide real verification. A roadmap item is never permission for an autonomous agent to make a broad architectural change.

The project is currently licensed under the repository’s [FSL-1.1-MIT license](LICENSE.md). Contributors and package authors must evaluate the current license text rather than assuming a conventional permissive open-source grant.

## What good roadmap-aligned work looks like

A strong contribution:

- Advances the earliest unblocked milestone or removes a demonstrated blocker.
- Reuses existing ownership and error-handling conventions instead of creating a second architecture.
- Completes a narrow observable contract, including real failure paths.
- Includes deterministic tests and an appropriate live smoke, capture, visual comparison, or benchmark.
- States CPU, GPU, memory, latency, build, dependency, and maintenance costs where relevant.
- Deletes the superseded path after every caller is migrated.
- Leaves documentation and diagnostics accurate enough for the next maintainer to reason about the system.

The roadmap should be revised when repository evidence, benchmark results, or product decisions change. It should not be updated merely to mark activity; capability status changes only when its exit criteria are demonstrably satisfied.
