# Architecture

## Structure

- `engine/core`: application lifecycle, logging, config, file IO, timing, camera, and compact math.
- `engine/platform`: GLFW-backed window and input handling with Vulkan surface creation.
- `engine/renderer`: renderer-facing data types: vertices, mesh generation, render stats, and renderer interface.
- `engine/renderer/vulkan`: concrete Vulkan 1.3 backend.
- `engine/shaders`: GLSL shaders compiled to SPIR-V by CMake through `glslc`.
- `samples/sandbox`: runnable demo application.

## Ownership and lifetime

`Application` owns the `Window`, `Camera`, `VulkanRenderer`, and frame clock. `VulkanRenderer` owns every Vulkan object it creates and destroys them in reverse dependency order. GPU buffers and images use explicit handle structs containing Vulkan handles plus VMA allocations, keeping lifetime and memory ownership visible while delegating memory-type selection and suballocation to VMA. Swapchain-dependent images and views are rebuilt during resize; graphics pipelines are reused when attachment formats are unchanged and recreated only when the HDR, depth, or swapchain format changes.

## Renderer architecture

The current backend is deliberately direct: a concrete Vulkan renderer implements `IRenderer` without hiding the API behind premature abstraction. The higher-level seams are already present: generated `MeshData` uploads into shared scene geometry buffers plus metadata-only `GpuMesh` submesh records, `SceneRenderList` carries renderer-facing mesh IDs/transforms/material constants/bounds without leaking Vulkan handles, camera state is passed through uniform buffers, per-frame scene-instance data is compacted into a storage buffer for mesh batching, frame statistics are exposed through `RenderStats`, immutable renderer/device capability metadata is exposed through `RenderDeviceInfo`, and `GpuResourceRegistry` keeps allocation/import accounting visible without owning Vulkan memory.

The CPU instrumentation keeps render-submit timing interpretable by splitting the existing post-acquire/pre-present window into four exclusive buckets: scene build, frame preparation, command recording, and queue-submit setup/submission. That scope starts after swapchain acquire and screenshot setup, ends after queue-submit bookkeeping, and excludes present/recreate handling plus screenshot readback waits. The prepare bucket covers visibility planning, mesh lookup, frustum/tile culling, visible-count capacity growth, and ImGui stats refresh; the command-record bucket maps to `recordCommandBuffer` materializing the prepared plan into instance/indirect buffers plus renderer-stat derivation, ImGui draw-data encoding, and Vulkan command emission.

Physical-device selection is explicit and diagnosable. `VulkanRenderer` evaluates each adapter into a small suitability report, ranks only adapters that satisfy the Vulkan 1.3 renderer contract, and preserves concrete rejection reasons for startup errors instead of failing with a generic "no suitable device" message.

The frame graph is currently a static metadata/validation layer, not a full transient-resource allocator. It keeps pass/resource intent visible, compiles a mode-specific DAG for either the default HDR-depth-write path or the optional depth-prepass path, and maps read/write usages to Vulkan image sync states (`layout`, stage, access) while explicit barrier emission remains in `VulkanRenderer`; the swapchain present layout is modeled as a final resource state rather than a fake pass write.

The GPU resource registry tracks persistent renderer-owned buffers/images and imported swapchain images by stable ID. It is intentionally diagnostic/accounting infrastructure, not a streaming resource manager yet; actual Vulkan lifetime still lives in explicit buffer/image handles and VMA allocations.
Renderer ownership is a clean cutover between startup and runtime. `VulkanRenderer` keeps explicit nullable handles for every Vulkan/VMA object, funnels destructor and constructor-failure teardown through the same non-throwing cleanup path, and only persists the pipeline cache during normal destruction. Subsystems with external global state, currently Dear ImGui, add local rollback around their backend initialization so failures before the renderer-owned initialized flag still release context/backend state.

Mesh batching uses an explicit enum-backed bucket key: visibility planning counts visible items per mesh bucket, while command recording computes prefix offsets from those counts and fills mesh-contiguous instance ranges with per-bucket write cursors before issuing scene work. The default path uses one `vkCmdDrawIndexedIndirect` multi-draw command per scene pass when `multiDrawIndirect` and `drawIndirectFirstInstance` are enabled at logical-device creation; the direct fallback remains one `vkCmdDrawIndexed` per visible mesh batch and is forceable with `--no-indirect-draws`.

Scene submission is intentionally data-oriented. `DemoSceneRenderer` reuses a CPU render list that reserves for the requested material-grid size. Before command recording, `VulkanRenderer` consumes mesh IDs and bounds to build a CPU-only visibility plan: frustum culling, visible mesh counts, grid-cache work, and the visible compacted instance count that drives per-frame descriptor-bound instance-buffer growth after that frame's fence. Command recording then materializes the plan into mesh-contiguous ranges, writes per-frame indirect commands when enabled, and emits scene pass commands without virtual scene dispatch.

Each frame starts with a 2048-visible-instance storage floor and can recreate/update its own storage-buffer descriptor when the visible compacted count exceeds that capacity; the CPU scene list may be much larger when grid culling leaves most items invisible. The current runtime switch is explicit and enum-backed: `DepthPrepassMode::ForceOn` (`--depth-prepass`) runs a depth-only pass before HDR shading, while `DepthPrepassMode::ForceOff` (`--no-depth-prepass`, the default) uses a depth-writing HDR scene pipeline for scenes where the prepass is not worth the extra geometry submission.

Texture loading is deliberately narrow but real: runtime assets are copied by CMake, `ImageLoader` decodes binary PPM/P6 into RGBA8 CPU pixels, and the Vulkan backend uploads the demo albedo texture on the graphics queue into a sampled sRGB image with generated mip levels when linear blits are supported. This avoids queue-family ownership complexity while the renderer still has separate graphics/transfer queues for buffer uploads.

The first image-based-lighting seam is procedural rather than texture-backed: `SceneUniforms` carries sky and ground environment colors/intensities, and the forward shader evaluates a hemispheric indirect term. This keeps the ABI and shader path ready for cubemap irradiance/prefilter maps without pretending those resources exist yet.

The render path is forward HDR with an optional depth prepass. The default path skips the prepass and clears/writes depth in the HDR scene pass because the current shader is geometry-cost dominated; `--depth-prepass` enables the two-pass variant for overdraw/material-cost experiments:

1. Optional: clear and populate the depth image in a depth-only dynamic-rendering pass.
2. Render generated scene geometry into an FP16 color image, either loading prepass depth with read-only `LESS_OR_EQUAL` testing or clearing/writing depth directly with `LESS` testing.
3. Sample the HDR image in a fullscreen triangle pass.
4. Apply ACES-style tonemapping and gamma correction into the swapchain image.
5. Draw the Dear ImGui stats overlay into the same swapchain dynamic-rendering pass.
6. Present.

## Major choices

- GLFW was selected because it is small, battle-tested, and fast to integrate for Vulkan surfaces on Linux and Windows.
- System Vulkan/GLFW/spdlog packages are used to keep the first checkout lightweight and deterministic on the workstation.
- Runtime sandbox flags configure frame count, resize smoke, dimensions, vsync, validation, GPU timestamp queries, and the optional ImGui diagnostics overlay without recompiling; window creation and programmatic resize extents are validated before crossing GLFW's `int` API boundary.
- A compact math layer avoids pulling GLM before the renderer needs broader math functionality.
- Vulkan dynamic rendering avoids render-pass/framebuffer boilerplate and matches Vulkan 1.3 usage.
- Uniform buffers are persistently mapped per frame to avoid hot-path map/unmap churn.
- Scene instance data is persistently mapped per frame and consumed from a storage buffer so visible items can batch by mesh without per-draw descriptor churn.
