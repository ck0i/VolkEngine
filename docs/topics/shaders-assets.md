# Shaders and assets

This page covers runtime file flow. Renderer behavior is in [Renderer pipeline](renderer-pipeline.md).

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

The Vulkan backend loads copied SPIR-V files from `EngineConfig::shaderDirectory` and rejects bytecode that is:

- smaller than a SPIR-V header.
- not a 32-bit word multiple.
- byte-swapped.
- missing magic `0x07230203`.

Valid bytes are copied into aligned `std::uint32_t` storage before `vkCreateShaderModule`.

## Shader hot reload

`--hot-reload-shaders` enables throttled polling of copied SPIR-V files. On change:

1. Build a full replacement pipeline set into local handles.
2. Swap the replacement set into use only after every pipeline/layout succeeds.
3. Retire the previous set after frame fences that could reference it signal.
4. Keep current pipelines live on rebuild failure.

## Pipeline cache

The backend stores the Vulkan pipeline cache at `${binaryDir}/cache/pipeline_cache.bin`.

- Loads only when the cache header matches vendor, device, and pipeline-cache UUID.
- Saves to a unique temp sibling first.
- Publishes only after a complete cache payload is read back and validated.
- Avoids truncating a previous cache on failed writes.

## Assets

Runtime assets are copied from `assets/` to `EngineConfig::assetDirectory`.

Current texture path:

- loads `assets/textures/ground_albedo.ppm`.
- decodes binary PPM/P6 into RGBA8 CPU pixels.
- accepts 8-bit and 16-bit samples with malformed-sample checks.
- uploads to a device-local sRGB sampled image on the graphics queue.
- generates mip levels with checked linear blits when supported.
- falls back to one mip level otherwise.
- enables sampler anisotropy only when the selected device exposes it.

The PPM loader is real but intentionally narrow; production image formats and streaming are future work.
