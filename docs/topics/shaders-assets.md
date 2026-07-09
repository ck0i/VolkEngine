# Shaders and assets

This page covers runtime file flow. Current ownership is split across these backend units:

- `VulkanRenderer.Pipelines.cpp` — SPIR-V validation/loading, pipeline set construction, pipeline cache load/save, and hot-reload rebuild/retire.
- `VulkanRenderer.Resources.cpp` — texture asset upload/sampler/descriptor setup and texture resource lifetime.
- `VulkanRenderer.Meshes.cpp` — generated geometry/mesh upload and shared vertex/index buffer ownership.
- `VulkanRenderer.Screenshot.cpp` — screenshot path and file-publish behavior.

Renderer behavior is in [Renderer pipeline](renderer-pipeline.md).

## Shader build flow

CMake finds `glslc` and compiles root shader files under `engine/shaders/`:

- `*.vert`
- `*.frag`

Outputs are written to `${binaryDir}/shaders/*.spv` with `--target-env=vulkan1.3`. Compiler depfiles track includes under `engine/shaders/common/`, so editing shared GLSL triggers the correct recompiles.

Important targets:

- `VolkEngineShaders` — SPIR-V outputs.
- `VolkEngineRuntimeShaders` — copies compiled SPIR-V beside `VolkEngineSandbox`.
- `VolkEngineAssets` — copies `assets/` to the build runtime asset directory.

## Runtime shader validation

`VulkanRenderer.Pipelines.cpp` loads copied SPIR-V bytecode from `EngineConfig::shaderDirectory` and rejects payloads that are:

- smaller than a SPIR-V header.
- not a 32-bit word multiple.
- byte-swapped.
- missing magic `0x07230203`.

Valid bytes are copied into aligned `std::uint32_t` storage before `vkCreateShaderModule`.

## Shader hot reload

`VulkanRenderer.Pipelines.cpp` owns `--hot-reload-shaders` polling. On change:

1. Build a full replacement pipeline set into local handles.
2. Swap the replacement set into use only after every pipeline/layout succeeds.
3. Retire the previous set after frame fences that could reference it signal.
4. Keep current pipelines live on rebuild failure.

## Pipeline cache

`VulkanRenderer.Pipelines.cpp` stores the Vulkan pipeline cache at `${binaryDir}/cache/pipeline_cache.bin`.

- Loads only when the cache header matches vendor, device, and pipeline-cache UUID.
- Saves to a unique temp sibling first.
- Publishes only after a complete cache payload is read back and validated.
- Avoids truncating a prior cache on failed writes.

## Assets

Runtime assets are copied from `assets/` to `EngineConfig::assetDirectory`.

Current texture path (`VulkanRenderer.Resources.cpp`):

- loads `assets/textures/ground_albedo.ppm`.
- decodes binary PPM/P6 into RGBA8 CPU pixels.
- accepts 8-bit and 16-bit samples with malformed-sample checks.
- uploads to a device-local sRGB sampled image on the graphics queue.
- generates mip levels with checked linear blits when supported.
- falls back to one mip level otherwise.
- enables sampler anisotropy only when the selected device exposes it.

Current generated geometry path (`VulkanRenderer.Meshes.cpp`):

- creates procedural cube/sphere/plane meshes in memory.
- merges mesh data into shared vertex/index buffers.
- uploads via staging and transfer/graphics upload commands during startup.

The PPM loader and generated-geometry paths are narrow by design; production image formats and streaming pipelines are still future work.
