# Architecture

This page is the entrypoint for high-level architecture docs.

- [Architecture topic](topics/architecture.md) — subsystem boundaries, ownership model, and renderer split summary.
- [Core API](api/core.md) — app/config/camera/time/helper contracts.
- [Platform API](api/platform.md) — window/input/surface contracts.
- [Renderer API](api/renderer.md) — renderer interface and stats/device metadata.

Renderer boundary note:
- Public renderer wiring stays at `VulkanRenderer.hpp` (`draw`, `stats`, `deviceInfo`, `requestScreenshot`, `waitIdle`; copy/move deleted).
- Internal Vulkan ownership stays private in `VulkanRenderer::Impl` plus the split `VulkanRenderer.*.cpp` units, anchored by `VulkanRendererImpl.hpp`.
- Do not model game-facing code around Vulkan handles; use `IRenderer`/`VulkanRenderer` ownership and contracts.

Start from [docs index](README.md) for the full map.
