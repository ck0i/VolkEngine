# VolkEngine docs

Use this directory as a map, not a novel. The headers are the source of truth for signatures; these pages document ownership, invariants, and the intended API boundary.

## Start here

- [Project roadmap](../ROADMAP.md) — verified current maturity, ordered capability milestones, exit criteria, and non-goals.
- [Architecture](topics/architecture.md) — subsystem boundaries, ownership, and frame data flow.
- [Renderer pipeline](topics/renderer-pipeline.md) — split backend notes for Vulkan startup, device/swapchain orchestration, frame flow, sync/upload paths, and screenshots.
- [Performance model](topics/performance.md) — hot-path rules, metrics, benchmark switches, and next work.
- [Shaders and assets](topics/shaders-assets.md) — shader build/copy flow, shader reload, texture loading, and pipeline cache.

## Public API reference

- [Core API](api/core.md) — `EngineConfig`, `RunOptions`, `Application`, timing, file IO, logging, math, camera.
- [Platform API](api/platform.md) — `Window` lifecycle, events, input, surface creation, resize signaling.
- [Renderer API](api/renderer.md) — `IRenderer`, `RenderDeviceInfo`, `RenderStats`, and backend-facing `VulkanRenderer` notes.
- [Scene API](api/scene.md) — `SceneRenderItem`, `SceneRenderList`, grid metadata, `DemoSceneRenderer`.
- [Frame graph API](api/frame-graph.md) — executable pass/resource contracts, logical lifetimes, barriers, and transient slots.
- [GPU resource registry API](api/resources.md) — diagnostic allocation accounting.

## Compatibility pages

The old top-level pages now point into the split documentation:

- [Architecture.md](Architecture.md)
- [Renderer.md](Renderer.md)
- [Performance.md](Performance.md)

## Documentation rules

- Keep top-level docs short and navigable.
- Put public contracts under `docs/api/`.
- Put backend behavior and rationale under `docs/topics/`.
- Do not duplicate implementation walkthroughs across API and topic pages.
- If a header changes a public type or invariant, update the matching API page in the same change.
