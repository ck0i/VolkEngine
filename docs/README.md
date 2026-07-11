# Documentation

Headers define signatures and data layout. These pages record ownership, invariants, and runtime behavior that are not obvious from declarations.

## Topics

- [Architecture](topics/architecture.md) — subsystem boundaries and frame data flow
- [Renderer pipeline](topics/renderer-pipeline.md) — Vulkan frame execution and synchronization
- [Performance](topics/performance.md) — hot-path constraints, metrics, and benchmark controls
- [Shaders and assets](topics/shaders-assets.md) — shader build/reload and authored asset flow

## API notes

- [Core](api/core.md) — application, jobs, world, time, and IO
- [Platform](api/platform.md) — window and input ownership
- [Renderer](api/renderer.md) — renderer-facing interface and telemetry
- [Scene](api/scene.md) — render data, authoring, cooking, partition, and landscape contracts
- [Frame graph](api/frame-graph.md) — resource declarations, hazards, lifetimes, and execution
- [GPU resource registry](api/resources.md) — diagnostic allocation accounting

Project setup is in the [root README](../README.md); priorities and unsupported areas are in the [roadmap](../ROADMAP.md).

Keep these pages short. Document stable contracts and non-obvious failure behavior; do not copy complete field lists or implementation walkthroughs from headers.
