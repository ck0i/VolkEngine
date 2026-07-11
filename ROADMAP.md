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

As of July 2026, the repository contains a C++23 static engine library and one GLFW/Vulkan sandbox.

### Engine and scene foundation

Implemented:

- Generational sparse-set ECS storage with guarded queries and deferred structural commands.
- Deterministic single-thread system scheduling.
- Fixed-capacity transactional typed events and timers.
- Fixed-step simulation, accumulated input, camera, time, and file helpers.
- Persistent UUID entity identity, validated scene hierarchy, fixed-step TRS interpolation, and deterministic CPU scene extraction.
- Bounded, transactional VESN v2 scene persistence for transforms, hierarchy links, and renderables.

Current limits:

- No job system, thread-safe world mutation model, reflection/schema system, or headless game application lifecycle.
- Scene persistence is an explicit component subset, not a generic authoring schema or optimized cooked-world format.
- No prefab model, undo/redo, asset GUID database, dirty extraction, spatial streaming, gameplay serialization, or editor metadata.
- No C# host, Lua integration, production input mapping, runtime UI contract, physics, audio, animation stack, navigation, networking, or replay system.

### Renderer foundation

Implemented:

- Vulkan 1.3 dynamic rendering with Vulkan Memory Allocator and two frames in flight.
- HDR rendering, ACES tone mapping, reverse-Z, and an adaptive depth prepass.
- Fixed albedo/normal/ORM PBR materials and one directional light.
- CPU frustum/grid visibility, sphere LOD selection, direct draws, and multi-draw indirect submission.
- Upload synchronization, pipeline caching, shader hot reload, GPU timestamps, renderer statistics, ImGui diagnostics, swapchain recovery, and PPM screenshots.
- CPU-side OBJ/procedural geometry, mesh cache/fetch optimization, mip generation, greedy voxel meshing, frame-graph topology and barrier intents, and diagnostic resource accounting.

Current limits:

- The renderer is single-view and forward-oriented, without scalable local lights, production shadows, image-based lighting, transparency, temporal antialiasing, upscaling, post effects, or robust device-loss policy.
- Descriptor-indexing capabilities may be enabled, but materials still use a fixed three-texture descriptor array. This is not a bindless material system.
- Visibility and LOD selection remain CPU-driven. There is no meshlet pipeline, GPU-generated draw stream, occlusion culling, or virtualized geometry.
- The frame graph plans topology, hazards, and barrier intent; it does not yet own pass execution, resources, transient allocation, or queue scheduling.
- There is no compressed/virtual texture pipeline, asynchronous residency system, scalable world renderer, hardware ray tracing, or production global illumination path.

### Assets, tools, and delivery

Implemented:

- OBJ and procedural geometry loading plus a fixed startup texture set.
- Linux and Windows CMake presets with pinned bootstrap dependencies.
- Seventeen CPU test executables plus the sandbox help test.
- Documentation for the public API, architecture, renderer pipeline, performance model, shaders, and assets.

Current limits:

- No glTF/FBX production importer, stable asset identity, dependency database, derived-data cache, platform cooking, asynchronous streaming, animation import, or material database.
- No scene editor, asset browser, viewport picking/gizmos, inspector, undo stack, play mode, project creation, packaging, installed SDK, plugin manifest, or package registry.
- No automated live-Vulkan CI, validation gate, visual regression, performance regression, or maintained external-style sample project.

### Verified baseline

At the time this roadmap was prepared:

- `ctest --preset linux-debug` passed all 18 registered tests.
- A three-frame Vulkan sandbox smoke run created a 1280×720 swapchain, loaded the current textures, compiled the existing frame-graph variants, rendered the adaptive depth/HDR/tone-map path through multi-draw indirect submission, wrote a screenshot, reported timings/statistics, and exited cleanly.
- Vulkan validation was requested but unavailable in that environment. The smoke run therefore does **not** establish validation-clean execution.

This baseline is evidence of a working renderer foundation, not evidence of a production-ready engine.

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

## S1. Executable frame graph and explicit resource ownership — **Next**

Turn the current metadata planner into the renderer’s execution and resource-lifetime backbone.

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

## S2. Asset identity, derived-data cache, and authored-scene import — **Next**

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

## S3. GPU-driven geometry and bindless material foundation — **Next**

Build the prerequisites for dense worlds before attempting virtualized geometry.

Scope:

- Replace the fixed texture array with a real capability-gated bindless resource model and stable material/resource handles.
- Cook meshes into meshlets or equivalent bounded clusters with hierarchy and bounds metadata.
- Move instance/cluster frustum culling, LOD selection, and indirect-command generation to GPU compute.
- Add depth-pyramid occlusion after the generated draw path is correct and measurable.
- Keep explicit fallbacks only where the Vulkan capability baseline requires them; do not preserve duplicate rendering architectures without evidence.
- Report visible and rejected instances/clusters, generated work, bandwidth, descriptor pressure, and CPU submission cost.

Exit criteria:

- A deterministic dense-scene benchmark renders many materials and instances without per-material descriptor rebinding or CPU draw enumeration.
- GPU culling and generated submission are correctness-tested against a bounded CPU reference on controlled scenes.
- Representative captures show no resource-lifetime, synchronization, descriptor, or visibility errors.
- Measurements demonstrate the scale at which the GPU-driven path outperforms the current CPU path; unsupported claims are not accepted.

## S4. Forward+ lighting, shadows, and production PBR baseline — **Next**

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

## S5. Job system, asynchronous IO, and profiling spine — **Next**

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

## S6. Reflected authoring model and interim editor foundation — **Next**

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

## M1. Unified streaming and world partition — **Later**

Depends on S2, S3, and S5.

- Use one dependency/residency scheduler for textures, geometry, world cells, animation, and audio.
- Add world partition cells, asynchronous dependency loading, residency budgets, eviction, origin management, and hierarchical LOD.
- Support both seamless geographic worlds and very high object density.
- Make loading priority, cancellation, backpressure, missing dependencies, and out-of-memory behavior explicit.
- Keep source authoring documents separate from optimized streamed runtime cells.

Exit gate: a deterministic traversal benchmark crosses a partitioned world without main-thread IO, unbounded residency, visible missing-cell gaps under the published budget, or unrecoverable partial loads.

## M2. Terrain, foliage, procedural generation, and atmosphere — **Later**

Depends on the streaming and GPU-driven foundations.

- Build terrain clip/LOD, layered landscape materials, collision/query data, and streamed editing/cooking.
- Add GPU-driven foliage placement, culling, wind, density tiers, and biome rules.
- Add deterministic procedural scatter/generation suitable for authoring and reproducible cooking.
- Add atmosphere, sky, clouds/weather, volumetrics, water, decals, and effects in measured increments needed by the benchmark.

Exit gate: the large natural-landscape benchmark is reproducible, traversable, visually stable, and fully instrumented under explicit scene, memory, loading, CPU, and GPU budgets.

## M3. Published fidelity and performance benchmark — **Later**

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
