# Shaders and assets

## Shader build

CMake compiles root `engine/shaders/*.vert`, `*.frag`, and `*.comp` files with `glslc --target-env=vulkan1.3`. Depfiles track includes under `engine/shaders/common/`.

- `VolkEngineShaders` builds SPIR-V under the build tree.
- `VolkEngineRuntimeShaders` copies SPIR-V beside the sandbox.
- `VolkEngineAssets` stages source assets.
- `VolkEngineRuntimeAssets` copies assets beside the sandbox.

At runtime, shader bytecode must contain a complete SPIR-V header, have a 32-bit-aligned size, use native byte order, and begin with magic `0x07230203`. Files load directly into aligned word storage.

## Pipeline reload and cache

With `--hot-reload-shaders`, changed files build a complete replacement pipeline set. Publication occurs only if every shader module, layout, and pipeline succeeds. The old set retires after relevant frame fences; failed inputs remain pending for retry with a 0.5–4 second backoff.

The cache at `${binaryDir}/cache/pipeline_cache.bin` loads only when vendor ID, device ID, and pipeline-cache UUID match. Save writes and validates a temporary sibling before replacement, preserving the previous cache on failure.

## Shader data

`scene.vert` reads packed instance/material data from the scene storage buffer. `scene.frag` consumes Forward+ tile lists, directional and local lights, shadow views, environment lighting, and reflection probes.

Material classes share one packed ABI and pipeline layout. Masked camera, depth, and shadow paths use the same alpha cutoff. Normal mapping orthonormalizes the tangent against the geometric normal and preserves handedness. Foliage wind is shared by camera, depth, and shadow vertex paths so silhouettes agree.

`light_assign.comp` assigns bounded submitted-light indices into fixed 16×16 tile partitions, up to 256 local lights and 64 entries per tile. `depth_pyramid.comp` builds the temporal reverse-Z pyramid. `atmosphere.frag` writes the analytic sky before scene geometry.

## Authored asset path

1. `SceneImporterRegistry` chooses a versioned importer by normalized extension.
2. The glTF importer produces scene, mesh, material, texture, and animation-metadata records.
3. Artifact content and dependencies produce derived-data keys.
4. `DerivedDataCache` validates and atomically publishes versioned artifacts.
5. The renderer resolves cooked handles and uploads a complete asset bundle.
6. Hot reload swaps the bundle at a main-thread frame boundary; any cook or upload failure retains the old bundle.

The glTF path supports hierarchy, local transforms, metallic-roughness materials, texture roles/color spaces, bounds, and validated animation clip/channel metadata. Animation samples and playback are not implemented.

Texture artifacts support decoded RGBA8, finite linear RGBA32F HDR, and non-supercompressed KTX2 BC1/BC3/BC7. BasisLZ, Zstd, and unsupported block formats fail before publication. The Vulkan upload path currently consumes RGBA8 artifacts; HDR and BC upload remains future work.

Relative asset paths are normalized under `EngineConfig::assetDirectory` and may not escape it. Absolute paths are explicit overrides.

## Texture upload

- Albedo uses sRGB format and gamma-correct mip handling where CPU generation is required.
- Normal maps use linear format and vector-renormalized CPU mips.
- ORM maps use linear format and straight channel averages.
- Albedo, normal, and ORM payloads share one startup staging submission.
- Decoded CPU mip data is released after staging.
- Bindless-capable devices use stable texture indices; other devices use fixed material descriptor sets.

## Geometry upload

The authored glTF path and generated landscape path feed the same imported mesh arrays. The OBJ override path additionally supports positive/negative indices, polygon fan triangulation, generated normals, tangent generation, finite-value validation, and malformed-input rejection.

CPU vertices retain full precision through import and tangent generation. Upload packs normal/tangent values to SNORM16, reorders indices for post-transform locality, remaps vertices for fetch locality, and writes all startup meshes into one shared staging buffer.

See [Renderer pipeline](renderer-pipeline.md) for frame use and [Scene API](../api/scene.md) for runtime records.
