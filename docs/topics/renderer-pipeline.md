# Renderer pipeline

Game code uses `IRenderer`. `VulkanRenderer` is the backend integration type and keeps all Vulkan handles private.

## Source ownership

| File | Responsibility |
| --- | --- |
| `VulkanRenderer.Device.cpp` | instance, adapter selection, device, queues, allocator, debug utilities |
| `VulkanRenderer.Swapchain.cpp` | surface formats, present mode, swapchain images/views, resize |
| `VulkanRenderer.FrameResources.cpp` | frame slots, fences, semaphores, timestamps, graph variants |
| `VulkanRenderer.Resources.cpp` | long-lived images/buffers, textures, samplers, descriptors |
| `VulkanRenderer.Meshes.cpp` | mesh construction and shared geometry upload |
| `VulkanRenderer.Pipelines.cpp` | shaders, layouts, pipelines, cache, hot reload |
| `VulkanRenderer.Uploads.cpp` | staging work and queue synchronization |
| `VulkanRenderer.Sync.cpp` | graph-to-Vulkan barriers and tracked image state |
| `VulkanRenderer.Visibility.cpp` | frustum/LOD/Hi-Z planning and generated draw work |
| `VulkanRenderer.Lighting.cpp` | light lists, cascades, shadow atlas, probes |
| `VulkanRenderer.Frame.cpp` | graph callbacks, command recording, submit, present |
| `VulkanRenderer.ImGui.cpp` | optional diagnostics/editor overlay |
| `VulkanRenderer.Screenshot.cpp` | readback and PPM publication |

`Lighting.cpp` contains backend-neutral validation and CPU reference implementations. `VmaUsage.cpp` defines the VMA implementation.

## Startup

1. Create instance, debug messenger, GLFW surface, and select a physical device.
2. Create the logical device, queues, VMA allocator, and command pools.
3. Create the swapchain and cached frame-graph variants.
4. Create long-lived render targets, descriptors, pipelines, frame resources, meshes, and textures.
5. Initialize optional ImGui state and report selected capabilities.

The backend requires Vulkan 1.3, swapchain support, dynamic rendering, synchronization2, graphics/present queues, and masked-fragment support. Adapter rejection reasons are logged.

## Frame

1. Wait for the frame-slot fence and retire slot-owned resources.
2. Read completed timestamp results.
3. Build visibility and lighting plans; grow frame storage only after its fence; update mapped data and descriptors.
4. Acquire a swapchain image.
5. Execute a cached graph variant into one primary command buffer.
6. Submit once to the graphics queue and present.

Work independent of the acquired image runs before acquisition. If pre-submit work fails afterward, the backend restores tracked state, recreates the swapchain, replaces the signaled acquire semaphore, requeues an uncompleted screenshot, and rethrows. Normal frames do not call `vkDeviceWaitIdle`.

## Passes

The graph selects depth-prepass on/off and screenshot on/off variants. The normal order is:

```text
GPU visibility → shadow atlas → optional depth
                                      ↓
                              reverse-Z Hi-Z build
                                      ↓
                             Forward+ assignment
                                      ↓
                    HDR scene → depth-tested atmosphere
                                      ↓
                               exposure + ACES
                                      ↓
                              optional ImGui/readback
                                      ↓
                                   present
```

Depth is reverse-Z: near maps to 1, far to 0, clear depth is 0, and tests use `GREATER` or `GREATER_OR_EQUAL`. Dynamic rendering is used instead of render-pass/framebuffer objects.

Forward+ uses 16×16 tiles, up to 256 local lights, and 64 light indices per tile. Tile-side frusta, spotlight sectors, and the current prepass's nearest/farthest opaque depths reject non-contributing lights. Devices without sampled-storage RG32 support retain far-depth culling; no-prepass and direct modes retain conservative frustum assignment. The 2048² shadow atlas has sixteen 512² slots in the first filtered-comparison depth format supported, preferring D16 before D32/D24: three directional cascades, then scene-ordered shadow-casting spot lights. Cascade transitions sample only adjacent directional slots; the final cascade fades analytically. Directional samples use affine clip coordinates without a perspective divide; spot samples retain perspective projection. Per-view indirect lists omit empty mesh batches; atlas overflow is counted rather than reallocating during the frame. Shadow-disabled and direct variants omit the atlas pass and sampling dependency.
Cascade transition starts are precomputed during lighting upload rather than reconstructed per fragment.

The HDR path selects filtered B10G11R11 when supported and falls back to RGBA16F; composition does not use alpha. It uses GGX/Smith/Schlick lighting, a complete equirectangular environment mip chain, up to four reflection probes, packed material classes, analytic atmosphere, and material-specific foliage/landscape/water branches. Atmosphere runs after opaque shading with a reverse-Z clear-depth equality test, so covered pixels do not invoke it. The terminal 1×1 environment radiance is uploaded with lighting uniforms for diffuse reuse; roughness selects directional specular mips. Tonemapping applies exposure, ACES, and exactly one sRGB encoding step.

## Scene submission

Meshes share device-local vertex and index buffers. Upload converts full-float import data into compact GPU vertices, reorders indices/vertices for cache locality, and packs integer material classes into existing instance metadata. Generated spheres omit zero-area pole primitives; generated landscape assets use the same mesh/material path as imported content.

The GPU path uploads cull candidates and instance records, performs frustum and optional cluster tests, selects sphere LOD, rejects against the previous depth pyramid, compacts visible instances, and emits indexed indirect commands. Built-in sphere transitions use projected-pixel thresholds; the 4×8 far mesh begins below a 4.32-pixel projected radius. Directional cascades use the same threshold in shadow-map space, while larger and spotlight-shadowed spheres retain an independent 8×16 proxy. Bindless textures and indirect drawing are capability-gated; unsupported devices use fixed descriptors and direct draws.

The current depth pyramid is half-resolution, reverse-Z, and preserves odd image edges. It is written after current depth rendering and read by the next frame.

## Swapchain and screenshots

Vsync selects FIFO. Without vsync, present mode preference is immediate, mailbox, then FIFO. Resize waits for a non-zero framebuffer and rebuilds image views, per-image semaphores, graph render targets, and dependent UI/pipeline state.

`requestScreenshot(path)` is latest-request-wins while pending. Readback writes P6 PPM to a temporary sibling and publishes only after the file is complete. Unsupported swapchain formats or missing transfer-source support report an error without affecting rendering.

See [Renderer API](../api/renderer.md), [Frame graph API](../api/frame-graph.md), and [Performance](performance.md).
