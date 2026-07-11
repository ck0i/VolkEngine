# VolkEngine roadmap

VolkEngine targets high-fidelity games on Windows and Linux, with a Vulkan renderer, C++ runtime, C# gameplay, and an editor suitable for small expert teams. Work is ordered by dependency, not date.

The first production target is a reproducible large-landscape benchmark. It must exercise terrain, foliage, object density, streaming, atmosphere, lighting, materials, and traversal under published frame and memory budgets.

## Engineering policy

- Measure CPU, GPU, memory, latency, and loading costs.
- Keep resource ownership, synchronization, and failure paths inspectable.
- Prefer compact data-oriented systems over speculative abstractions.
- Complete authoring-to-runtime workflows before adding isolated features.
- Break pre-1.0 APIs when necessary and migrate every in-tree caller.
- Add third-party code only when its license and maintenance costs are understood.

## Current state

The repository contains a C++23 engine library, Vulkan sandbox, interim ImGui editor, and streamed-landscape benchmark.

### Runtime and world

- Generational sparse-set ECS with guarded queries and deferred structural commands.
- Fixed-step simulation, input snapshots, typed events and timers.
- Dependency-aware job system with work stealing, cancellation, cooperative waits, and profiling.
- Stable scene identities, hierarchy, TRS interpolation, reflection metadata, authoring documents, cooked worlds, and runtime instantiation.
- Unified artifact residency and hierarchical world partition with budgets, retained frontiers, and local-origin rebasing.

Limits: world mutation is not generally thread-safe; reflection covers built-in authoring components; partition publication rebuilds a complete CPU `World`; prefabs and gameplay serialization are absent.

### Renderer

- Vulkan 1.3 dynamic rendering, VMA, reverse-Z, HDR output, ACES tone mapping, and adaptive depth prepass.
- Forward+ tiled lights, directional cascades, local spot shadows, environment lighting, reflection probes, and ten packed material classes.
- Capability-gated bindless textures and GPU frustum/LOD/temporal-occlusion culling with indirect submission; CPU fallbacks remain for unsupported devices.
- Executable frame graph, upload synchronization, pipeline cache, shader reload, timestamp queries, diagnostics, swapchain recovery, and screenshots.
- Procedural landscape, water, analytic atmosphere, and foliage wind use the normal scene and lighting paths.

Limits: single view; no transparency, TAA/upscaling, general post-processing, volumetrics, hardware ray tracing, production GI, virtual textures, multi-queue frame-graph scheduling, or physical transient-memory aliasing. GPU cluster submission is flat rather than hierarchical.

### Assets and tools

- Stable asset IDs, dependency records, content-addressed derived data, atomic versioned artifacts, and transactional reload.
- glTF 2.0 scenes, meshes, hierarchy, metallic-roughness materials, textures, bounds, and animation metadata.
- RGBA8, HDR RGBA32F, and non-supercompressed KTX2 BC1/BC3/BC7 artifact support.
- Reflected authoring document, undo/redo, hierarchy, inspector, viewport, cooking, and runtime publication in a separable editor build.
- Deterministic terrain fields, brushes, terrain LOD patches, biome foliage, and water cooking.

Limits: no FBX path, animation sample/playback pipeline, asset browser, prefab workflow, project format, installed SDK, packaging, or plugin/package system. HDR and BC artifacts are not yet uploaded through the Vulkan runtime path.

The maintained test and benchmark gates cover the frame graph, renderer synchronization, asset cooking, GPU visibility, lighting/materials, jobs, editor workflow, world partition, and streamed landscape. Results from one driver are not cross-hardware proof; repeatable controlled GPU coverage remains required.

## Milestones

Status terms:

- **Current** — implemented and covered by an in-tree test or benchmark gate.
- **Next** — active dependency chain.
- **Later** — approved direction blocked by earlier work.
- **Research** — requires a measured prototype before integration.

### S1. Executable frame graph — Current

The renderer executes Forward+ assignment, visibility, shadow, depth, HDR, depth-pyramid, tone-map, and readback passes through compiled graph variants. The graph validates resource use, derives hazards and lifetimes, and exposes barriers, transient slots, and timings. Vulkan owns physical allocation and command emission.

### S2. Authored asset pipeline — Current

Stable asset identity, deterministic glTF import, dependency-aware cooking, derived-data caching, and transactional runtime publication replace source-file loading in the authored scene path. Corrupt, stale, cyclic, and incompatible inputs have test coverage.

### S3. GPU-driven geometry — Current

Meshes are cooked into bounded clusters. Compute culls instances/clusters, chooses LOD, uses prior-frame Hi-Z, compacts visible instances, and generates indirect commands. Bindless texture indexing is capability-gated. Dense-scene tests compare generated work with a CPU reference; controlled measurements identify where this path wins.

### S4. Forward+ and PBR baseline — Current

The renderer supports bounded point and spot lights, one directional light, cascaded and local spot shadows, HDR environment lighting, reflection probes, and packed standard/masked/clear-coat/foliage/skin/hair/cloth/emissive/landscape/water materials. Overflow and atlas pressure are reported.

### S5. Jobs and asynchronous IO — Current

A fixed-capacity scheduler handles dependencies, cooperative waits, cancellation, failure propagation, and job telemetry. Simulation preserves serial mutation boundaries while independent read-only systems may run in parallel. Asset cooking and residency use the same scheduler without normal-frame waits.

### S6. Authoring model and interim editor — Current

Stable reflection metadata drives versioned authoring data, property editing, migration, undo/redo, cooking, and runtime instantiation. The editor provides hierarchy, inspector, viewport picking/gizmos, asset import, and profiling. Runtime builds exclude editor and ImGui sources.

### M1. Streaming and world partition — Current

One residency path handles texture, geometry, world-cell, animation, and audio artifacts. Hierarchical cells load asynchronously under an explicit byte budget. Complete replacement frontiers publish atomically; failed or incomplete frontiers retain existing coverage. Origin rebasing and traversal metrics are part of the benchmark contract.

### M2. Landscape benchmark — Current

Deterministic terrain, water, and biome foliage cook into ordinary streamed cells and renderer assets. The traversal gate checks visible content, timing samples, residency budget, publications, origin shifts, and zero coverage or loading failures.

### M3. Published fidelity and performance benchmark — Next

Target: native 3840×2160 at 60 FPS on an RTX 4070 or RX 7800 XT class GPU with a modern eight-core CPU. A 120 FPS mode must publish separate resolution, content, and feature budgets.

The benchmark must report:

- hardware, driver, build, scene revision, resolution, and settings;
- median, p95, and p99 CPU/GPU frame times and hitch counts;
- host/device memory, residency, upload, and streaming pressure;
- loaded and visible instances, triangles/clusters, materials, lights, dispatches, and draws;
- startup, loading, traversal, and cell latency;
- reference images and temporal-stability evidence.

Missing the budget requires changing content, quality, architecture, or the published target—not hiding cost in defaults.

### M4. C# gameplay and application framework — Later

Embed modern .NET with generated versioned bindings. Expose entities, components, systems, events, input, assets, scenes, and profiling to gameplay code while retaining C++ for engine and performance-critical extensions. Add project/game lifecycle, headless execution, world transitions, settings, save data, and assembly reload with defined state-loss behavior.

Exit condition: a separate sample project implements gameplay in C#, reloads in the editor, runs headless tests, and packages without engine source changes.

### M5. Input and runtime UI — Later

Add action maps, contexts, rebinding, gamepad hot-plugging, chords, dead zones, sensitivity, and accessibility controls. Define UI services for rendering, text, input/focus, clipping, localization, animation, and lifetime; prove them with a replaceable reference backend.

### M6. Minimum game stack — Later

- **Animation:** skeletal import, GPU skinning, clips, blending, state machines, events, and root motion.
- **Physics:** collision, queries, rigid bodies, triggers, and character control integrated with jobs, transforms, and serialization.
- **Audio:** spatial playback, streaming, attenuation, buses, mixing, editor preview, and middleware extension points.
- **Gameplay:** navigation, save/load, sequencing, debug, and runtime services required by the maintained sample.

Exit condition: the sample demonstrates input, UI, C# gameplay, animation, character interaction, audio, save/load, editor iteration, headless tests, and packaged Windows/Linux execution.

### M7. Project SDK and packaging — Later

Support both installed-SDK projects and source workspaces. Define project manifests, dependency resolution, build profiles, asset cooking, packaging, logs/crash artifacts, and launch/debug workflows. Maintain an external-style sample as a release gate.

## Research and long-term work

### Virtualized geometry — Research

Prototype a streamed cluster hierarchy only after the existing meshlet, culling, and streaming paths provide a stable comparison. Integration requires gains in geometry density, traversal cost, memory, build time, and image stability.

### Ray tracing and global illumination — Research

Rasterized Forward+ remains the baseline. Evaluate ray-traced reflections, shadows, and GI in that order, each with a published frame/memory budget and a scalable fallback. A path-traced reference mode may validate materials and lighting before it is a runtime mode.

### Renderer depth — Later

Virtual textures, TAA, upscaling, dynamic quality, post effects, transparency, decals, particles, reflections, volumetrics, weather, advanced water, cinematics, dense-city workflows, and mature HLOD follow measured benchmark needs.

### Native editor and extensions — Later

Replace the interim ImGui shell when real workflows justify a retained native UI. Preserve document, command, reflection, viewport, asset, and package contracts across that change. Begin with local versioned plugin/package manifests; registry, provenance, trust, and marketplace work follows proven local package loading.

### Production 1.0 — Later

Version 1.0 requires:

- a small team can author, profile, test, package, and ship a polished game;
- controlled Windows/Linux build, runtime, validation, visual, and performance coverage;
- an external sample using C# gameplay, cooked assets, editor workflows, runtime UI, core game systems, and packaging;
- documented native and managed APIs, failure handling, migration policy, budgets, ownership, and supported hardware.

## Non-goals before 1.0

- Mobile and web targets.
- Console commitments without SDK access and dedicated ownership.
- Networking, replication, rollback, or online services before a project requires them.
- Permanent ImGui-based end-user tooling.
- Compatibility shims for evolving pre-1.0 APIs.
- A marketplace before local package contracts work.
- Renderer research that bypasses benchmark prerequisites.
- Feature-count work that does not complete an authoring, runtime, debugging, or delivery workflow.

See [CONTRIBUTING.md](CONTRIBUTING.md) before proposing roadmap work. Update milestone status only when repository evidence satisfies the stated contract.
