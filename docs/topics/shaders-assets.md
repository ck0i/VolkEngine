# Shaders and assets

This page covers runtime file flow. Current ownership is split across these backend units:

- `SceneImporter.cpp`, `GltfImporter.cpp`, and `TextureArtifact.cpp` — importer dispatch, authored-scene conversion, and engine-native texture validation/serialization.
- `ReferenceAssetPipeline.cpp`, `AssetDatabase.cpp`, and `DerivedDataCache.cpp` — dependency records, deterministic cooking, cache publication, and transactional reload.
- `VulkanRenderer.Pipelines.cpp` — SPIR-V validation/loading, pipeline set construction, pipeline cache load/save, and hot-reload rebuild/retire.
- `VulkanRenderer.Lighting.cpp` — Forward+ descriptors, bounded tile/light uploads, and directional/local shadow camera preparation.
- `VulkanRenderer.Resources.cpp` — texture asset upload/sampler/descriptor setup and texture resource lifetime.
- `VulkanRenderer.Meshes.cpp` — procedural geometry, imported OBJ mesh loading, mesh upload, and shared vertex/index buffer ownership.
- `VulkanRenderer.Screenshot.cpp` — screenshot path and file-publish behavior.

Renderer behavior is in [Renderer pipeline](renderer-pipeline.md).

## Shader build flow

CMake finds `glslc` and compiles root shader files under `engine/shaders/`:

- `*.vert`
- `*.frag`
- `*.comp`
Outputs are written to `${binaryDir}/shaders/*.spv` with `--target-env=vulkan1.3`. Compiler depfiles track includes under `engine/shaders/common/`, so editing shared GLSL triggers the correct recompiles.

Important targets:
- `VolkEngineShaders` — SPIR-V outputs in the build tree.
- `VolkEngineRuntimeShaders` — copies compiled SPIR-V beside `VolkEngineSandbox`.
- `VolkEngineAssets` — stages source assets into the build tree.
- `VolkEngineRuntimeAssets` — copies assets beside `VolkEngineSandbox`, matching `EngineConfig::assetDirectory`.

## Runtime shader validation

`VulkanRenderer.Pipelines.cpp` loads copied SPIR-V bytecode from `EngineConfig::shaderDirectory` and rejects payloads that are:

- smaller than a SPIR-V header.
- not a 32-bit word multiple.
- byte-swapped.
- missing magic `0x07230203`.

Valid SPIR-V is read directly into aligned `std::uint32_t` storage before `vkCreateShaderModule`, avoiding a byte-vector load plus copy on startup and hot reload.

## Scene shader data flow

`scene.vert` reads packed instance material constants and texture indices from the scene SSBO and emits material values as flat fragment inputs while position, normal, UV, and tangent remain interpolated. `scene.frag` consumes the per-frame Forward+ tile lists, directional/local light records, shadow views/atlas, environment map, and bounded reflection-probe array through the shared lighting descriptor set.

The fragment path applies GGX/Smith/Schlick direct light, inverse-square/range-window point and spot attenuation, stable directional cascade selection, local spot-shadow lookup, roughness-dependent environment LOD, diffuse environment irradiance approximation, probe blending, and ambient occlusion in linear HDR. Standard, masked, clear-coat, foliage, skin, hair, cloth, and emissive responses share one material-class ABI and pipeline layout. Base-color alpha discard is legal because device selection explicitly requires/enables Vulkan 1.3 `shaderDemoteToHelperInvocation`.

Normal-mapped fragments orthonormalize the tangent against the interpolated normal, derive bitangent handedness from `vWorldTangent.w`, and reuse the normalized geometric normal as the TBN matrix's third axis. Material availability comes from separately packed texture-mask bits; `RenderMaterial::flags.y/z/w` carry class, class strength or alpha cutoff, and normal strength.

`light_assign.comp` uses one workgroup per 16×16 tile. It derives a conservative tile frustum, tests a bounded 256-light array, and writes each tile's fixed 64-index partition plus an exact overflow counter. `depth_pyramid.comp` remains the temporal reverse-Z reduction path.

Camera depth uses `scene_depth.vert`; opaque casters omit a fragment stage, while masked casters add `shadow.frag` so the prepass preserves authored alpha cutoff. Shadow atlas rendering uses `shadow.vert` for both caster classes and adds `shadow.frag` only for masked geometry; push constants select the shadow-view matrix and packed atlas scale/bias.

## Shader hot reload

`VulkanRenderer.Pipelines.cpp` owns `--hot-reload-shaders` polling. On change:

1. Build a full replacement pipeline set into local handles.
2. Swap the replacement set into use only after every pipeline/layout succeeds.
3. Retire the previous set after frame fences that could reference it signal.
4. Keep current pipelines live on rebuild failure.
5. Leave failed shader timestamps unacknowledged so a corrected file is retried; retries back off from 0.5 to 4 seconds to avoid rebuilding continuously while an editor writes an invalid intermediate file.

## Pipeline cache

`VulkanRenderer.Pipelines.cpp` stores the Vulkan pipeline cache at `${binaryDir}/cache/pipeline_cache.bin`.

- Loads only when the cache header matches vendor, device, and pipeline-cache UUID.
- Saves to a unique temp sibling first.
- Publishes only after a complete cache payload is read back and validated.
- Avoids truncating a prior cache on failed writes.

## Assets

Runtime assets are copied from `assets/` to `EngineConfig::assetDirectory`.
The reference authored-scene path runs through `ReferenceAssetPipeline` before renderer upload:

- `AssetId` supplies stable 128-bit identities; the transactional asset database records source/importer/settings hashes, dependencies, artifact keys, target, state, and diagnostics.
- `SceneImporterRegistry` owns importer identity, version, normalized extensions, and the import function. Registration and lookup are deterministic; duplicate IDs/extensions and unknown source extensions fail before source mutation. The built-in cgltf importer handles `.gltf` and `.glb`.
- `GltfImporter` imports glTF 2.0 mesh primitives, hierarchy/local transforms, metallic-roughness materials, texture roles/color spaces, bounds, and animation clip/channel metadata. Animation metadata validates target nodes, interpolation, accessor shape, finite output values, strictly increasing non-negative keyframe times, and configured size limits; sample payloads and playback remain future work.
- Mesh, material, scene, and texture artifacts have independent schema versions. Derived-data keys use serialized artifact content plus dependency keys, so animation-only edits rebuild only the scene and source-byte changes that decode identically reuse the existing texture/dependent artifacts.
- `TextureArtifact` decodes ordinary sources to RGBA8, preserves Radiance HDR as finite linear RGBA32F, and validates non-supercompressed KTX2 BC1/BC3/BC7 dimensions, color space, mip sizes, ranges, and non-overlap. BasisLZ/Zstd KTX2 and unsupported GPU block formats fail actionably rather than publishing partial cache state.
- `DerivedDataCache` verifies headers, schema, payload sizes, and content hashes and publishes through atomic temporary files. `ReferenceAssetReloader` cooks a candidate bundle and publishes it only after the complete database and artifacts validate.
- The sandbox resolves the cooked reference scene and RGBA8 texture artifacts through this path; it no longer decodes source image bytes during renderer startup. Cache/rebuild counts and cook latency are included in the machine-readable run summary. HDR and BC artifacts remain available for a future capability-aware Vulkan upload path.

The direct `EngineConfig` texture and OBJ settings remain explicit fallback/override paths.

Configured ground texture paths (`EngineConfig::groundAlbedoTexture`, `groundNormalTexture`, and `groundOrmTexture`) resolve relative to `EngineConfig::assetDirectory`; relative paths are normalized and rejected when they escape the asset root, while absolute paths remain accepted as direct overrides:

- defaults load `textures/ground_albedo.png`, `textures/ground_normal.png`, and `textures/ground_orm.png` through `loadImageRgba8()`.
- startup validates that each configured role path is non-empty and names a regular file before allocating upload resources.
- uses stb_image for non-PPM image formats and keeps `loadPpmRgba8()` for PPM fixtures/compatibility.
- decodes source images to RGBA8 CPU pixels while preserving source alpha.
- uploads albedo as `VK_FORMAT_R8G8B8A8_SRGB`.
- uploads normal maps as linear `VK_FORMAT_R8G8B8A8_UNORM`; shader code remaps sampled normals from `[0, 1]` to `[-1, 1]` before TBN transform.
- uploads packed ORM material maps as linear `VK_FORMAT_R8G8B8A8_UNORM`; `scene.frag` reads R as ambient occlusion, G as roughness multiplier, and B as metallic multiplier.
- uses explicit mip policies: albedo may use GPU blit mips for opaque textures and falls back to gamma-correct alpha-weighted CPU mips when alpha coverage or sRGB blit support requires it; packed linear scalar maps such as ORM use straight RGBA averaging for CPU fallback so scalar RGB channels never get alpha-weighted; normal maps always use CPU vector-renormalized mips.
- builds normal-map mip chains on the CPU by decoding each proportional source footprint to tangent-space vectors, averaging, renormalizing, preserving averaged alpha, and uploading explicit mip copy regions; this avoids color-style byte averaging that flattens high-frequency normals and covers odd-size edge texels.
- records albedo, normal, and ORM texture copies/mip generation from one shared staging buffer into one startup graphics upload command buffer and one submit, then binds them through one fixed material texture descriptor array.
- uses separate descriptor samplers for color/scalar maps and normal maps: albedo/ORM can enable device anisotropy and use generated material mip ranges, while normal maps use their explicit CPU-renormalized mip range with anisotropy disabled.

Current geometry path (`VulkanRenderer.Meshes.cpp`):
- creates procedural cube/sphere/plane meshes in memory.

- resolves `EngineConfig::importedModelPath` relative to `EngineConfig::assetDirectory`, normalizes it, and rejects relative paths that escape the asset root; absolute overrides are accepted.
- validates the resolved model path is a regular file before mesh allocation, then loads it through `loadObjMesh()`.
- supports Wavefront OBJ `v`, `vt`, `vn`, and `f` records, positive/negative face indices, `v`, `v/vt`, `v//vn`, `v/vt/vn` tuples, polygon fan triangulation, deduped vertex tuples, generated normals when faces omit normals, and generated-normal fallback when explicit OBJ normals are degenerate or cannot be safely normalized; all OBJ numeric attributes must be finite, and malformed/non-finite values are rejected before mesh bounds, tangent generation, or GPU upload.
- computes `MeshData::bounds` from vertex positions with double-precision intermediates, rejecting finite coordinates whose derived center/radius cannot be represented as a finite float; only then computes `Vertex::tangent` as `vec4(xyz, handedness)` for normal-map TBN shading. Missing/degenerate UVs get deterministic fallback tangents.
- packs per-instance normal matrices as three `vec4` columns so shaders transform normals with inverse-transpose data while tangents still use the model linear transform and `tangent.w * sign(det(model3x3))`.
- converts triangle-list CPU `MeshData` vertices into a compact Vulkan `GpuVertex` stream: full-float position/UV plus `R16G16B16A16_SNORM` normal and tangent attributes; uploaded indices are reordered for post-transform vertex-cache locality, then vertices are remapped to first-use order for vertex-fetch locality.
- writes all startup mesh vertex/index payloads directly into one mapped staging buffer, then submits one transfer/graphics upload command during startup.

The source path accepts common stb_image-backed formats, Radiance HDR, and bounded non-supercompressed KTX2 BC1/BC3/BC7 artifacts. Material libraries, BasisLZ/Zstd transcoding, runtime HDR/BC upload, and streaming remain future work.
