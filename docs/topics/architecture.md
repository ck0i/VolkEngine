# Architecture

VolkEngine separates runtime state, authoring data, renderer-facing scene data, and Vulkan implementation details. Public engine code does not handle Vulkan objects.

## Subsystems

| Path | Responsibility |
| --- | --- |
| `engine/core` | Application lifecycle, jobs, ECS, simulation scheduling, time, camera, input-independent utilities |
| `engine/platform` | GLFW lifetime, windows, native events, input snapshots, Vulkan surface creation |
| `engine/assets` | Asset identity, import, dependency records, derived data, runtime residency |
| `engine/scene` | Reflection metadata, persisted scenes, cooked worlds, world partition |
| `engine/landscape` | Terrain field queries and terrain/foliage/water cooking |
| `engine/renderer` | Renderer interface, scene extraction, lighting ABI, frame graph, resource accounting |
| `engine/renderer/vulkan` | Vulkan device, resources, pipelines, synchronization, and frame submission |
| `engine/editor` | Authoring document, commands, cooking, and optional ImGui UI |
| `samples` | Sandbox, editor executable, and partition benchmark |

## Ownership

- `Application` owns `JobSystem`, platform state, camera, active assets, renderer, and clocks. Member order keeps assets and the window alive through renderer teardown and joins background asset work before destroying the scheduler.
- `GlfwRuntime` owns process-wide GLFW initialization. `Window` owns one native window and remains on the main thread.
- `World` owns entities and component pools. Structural mutation is serial; `WorldSystemScheduler` may run independent read-only systems in parallel and applies deferred commands at a step boundary.
- `ResidencyManager` borrows the application job system. It owns request state and resident bytes; worker tasks perform IO but do not mutate manager state.
- `WorldPartitionRuntime` owns the active cell frontier and candidate state. A candidate commits only after a complete `CookedWorld` is instantiated successfully.
- `AuthoringDocument` is editor-owned source data. `CookedWorld` is the runtime format. Runtime builds retain reflection and cooked-world loading but exclude editor sources.
- `VulkanRenderer` owns a private `Impl`. Buffers and images pair native handles with VMA allocations. The frame graph owns logical lifetimes; Vulkan code realizes physical resources.

Non-owning callbacks and their contexts must outlive the operation that uses them. This applies to jobs, simulation systems, application update hooks, and renderer overlays.

## Frame flow

1. Poll platform events and produce an `InputState` snapshot.
2. Update the camera at render rate.
3. Advance streaming and publish a complete replacement world, if available.
4. Convert wall time into fixed simulation steps. Each successful step runs systems, applies deferred commands, and captures transform history.
5. Interpolate local transforms, resolve hierarchy, and build a reusable `SceneRenderList`.
6. Validate visibility and lighting inputs; prepare frame-slot buffers.
7. Execute the selected frame-graph variant and submit one primary graphics command buffer.
8. Publish renderer, job, residency, and benchmark statistics.

Failure before world publication leaves the old world active. Simulation failure invalidates presentation history. Renderer failure after image acquisition restores tracked state and recreates swapchain ownership before the acquire semaphore can be reused.

## Renderer boundary

- `Renderer.hpp` is the game-facing contract.
- `vulkan/VulkanRenderer.hpp` is the backend integration seam.
- `VulkanRenderer.cpp` forwards to `VulkanRenderer::Impl`.
- Split `VulkanRenderer.*.cpp` files own device, swapchain, resources, pipelines, uploads, lighting, visibility, frame execution, diagnostics, and screenshots.
- `VmaUsage.cpp` is the only translation unit defining `VMA_IMPLEMENTATION`.

Current constraints: one Vulkan backend, one view, serial world mutation, CPU-built partition publication, and no GPU page-based scene representation.
