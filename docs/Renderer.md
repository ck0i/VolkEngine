# Renderer

This page is split into API contracts and implementation internals:

- [Renderer API](api/renderer.md) — public contracts (`IRenderer`, `RenderDeviceInfo`, `RenderStats`).
- [Renderer pipeline](topics/renderer-pipeline.md) — backend runtime behavior and the Vulkan backend source split.
- [Scene API](api/scene.md) — render-list records, material-grid metadata, demo scene builder.
- [Frame graph API](api/frame-graph.md) — pass/resource metadata and validation.
- [Shaders and assets](topics/shaders-assets.md) — shader build/copy/reload and asset loading.

Application/game code should use `IRenderer` and the API docs above for rendering calls.
`VulkanRenderer` is the backend-specific integration type (`engine/renderer/vulkan` implementation of `IRenderer`) used when wiring backend choice directly; it is not the normal public app contract.
