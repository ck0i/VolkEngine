# Scene API

Headers: `engine/renderer/SceneRenderer.hpp`, `engine/scene/SceneReflection.hpp`,
`engine/scene/CookedWorld.hpp`, `engine/landscape/Landscape.hpp`; implementation
spans the corresponding source files plus editor-only authoring code under
`engine/editor`.

The scene API provides renderer-facing data, a CPU-only `WorldSceneExtractor`
bridge, stable reflected authoring metadata, and a deterministic cooked-world
boundary. Vulkan remains behind the renderer boundary.

## `SceneMeshId`

```cpp
enum class SceneMeshId : std::uint8_t {
    Cube,
    Sphere,
    GroundPlane,
    ImportedModel
};
```

Logical mesh choices understood by the demo renderer. The Vulkan backend maps these to internal mesh/LOD buckets.

## `RenderMaterial`

```cpp
struct alignas(16) RenderMaterial {
    Vec4 albedoRoughness;
    Vec4 emissiveMetallic;
    Vec4 flags;
};
```

Shader-facing material constants. Layout is intentionally vec4-packed for storage-buffer transfer.

Current packing:

- `albedoRoughness.rgb`: base color.
- `albedoRoughness.a`: roughness.
- `emissiveMetallic.rgb`: emissive color.
- `emissiveMetallic.a`: metallic.
- `flags.x`: integral `MaterialFeature` bits (`AlphaMask`, `DoubleSided`,
  `GroundGrid`) with a quantized alpha cutoff in bits 8–23 for masked draws.
- `flags.y`: integral `RenderMaterialClass` ABI value (`Standard`, `Masked`,
  `ClearCoat`, `Foliage`, `Skin`, `Hair`, `Cloth`, `Emissive`, `Landscape`, or
  `Water`).
- `flags.z`: class-response strength; foliage uses it as wind amplitude.
- `flags.w`: normal-map strength in `[0, 4]`.

Texture availability is encoded separately from this vec4 by the three `TextureAssetHandle` roles (base color, normal, ORM). Materials without an authored role receive the renderer's valid reference fallback handle; this preserves one descriptor/instance ABI for generated materials. Invalid, non-integral class/feature values and negative class/normal parameters are rejected before renderer borrowing.

## Lighting and environment records

`Lighting.hpp` defines fixed CPU/GLSL ABI records and bounds:

- `RenderDirectionalLight`: normalized world-space direction, linear color/intensity, and a shadow-enable bit. The renderer fits three practical-split reverse-Z cascades.
- `RenderLocalLight`: point/spot type, world-space position/range, linear color/intensity, spot direction/cones, and shadow intent. A scene accepts at most 256 lights; invalid ranges, cones, directions, colors, and non-finite values fail before submission.
- `RenderEnvironment`: neutral sky/ground tint-intensity multipliers plus positive exposure compensation and finite equirectangular rotation. Renderer-owned maximum mip and active-probe count replace the corresponding shader-facing fields each frame.
- `RenderReflectionProbe`: world-space center, positive blend radius, and finite non-negative tint/intensity. At most four spherical probes are accepted.

Forward+ uses 16×16 screen tiles with at most 64 local-light indices per tile. `buildTiledLightLists()` is the deterministic CPU reference contract used by tests; the Vulkan backend records one GPU assignment dispatch and reports exact bounded overflow. `assignShadowAtlasSlots()` reserves three of sixteen 512² atlas slots for directional cascades, then assigns shadow-casting spot lights in scene order and reports overflow.

## `SceneRenderItem`

```cpp
struct SceneRenderItem {
    Vec3 boundsCenter;
    float boundsRadius;
    SceneMeshId mesh;
    Mat4 model;
    RenderMaterial material;
};
```

Contract:

- Bounds must conservatively enclose the transformed item.
- `boundsCenter`, `boundsRadius`, and `mesh` stay at the front of the record for visibility planning cache locality.
- Static assertions lock size/offsets; update shader/renderer docs if the layout changes.

## `SceneRenderList`

A reusable vector-backed render-list plus optional material-grid metadata. `WorldSceneExtractor` owns and reuses its list; callers may borrow the `const SceneRenderList&` returned by `build()` until the next build or extractor destruction. The renderer borrows it synchronously for one draw call and does not retain it.
Lighting is owned by the list as frame data: `setLocalLights`, `setDirectionalLight`, `setEnvironment`, and `setReflectionProbes` validate a complete replacement before committing it; their accessors return read-only views. `clear()` also restores default directional/environment state and removes local lights and probes.
List operations:

- `clear()` — clears items, lighting/probes, and grid metadata; restores default directional/environment records.
- `reserve(capacity)` — preserves capacity for repeated scenes.
- `push(item)` — appends one item and invalidates grid tiles if the append lands inside the declared grid range.
- `size()`, `capacity()`, `empty()`.
- `operator[](index) const` — read-only item access.
- `operator[](index)` — mutable access; invalidates grid tiles when mutating an item inside the material-grid range.
- `begin()`, `end()` — const pointer iteration.

Material-grid metadata:

- `setMaterialGridRange(firstItem, rows, columns)` — declares a contiguous row-major grid inside the item list.
- `rebuildMaterialGridTiles(tileRows, tileColumns)` — rebuilds tile bounds/homogeneity metadata when dimensions are valid.
- `materialGridRange()` — returns declared range.
- `materialGridTiles()` — returns cached tile records.
- `materialGridTilesCoverRange()` — true only after a valid rebuild covers the full range.
- `materialGridTileRevision()` — increments when tile metadata is invalidated.

If grid metadata is missing or stale, the renderer falls back to generic per-item visibility scanning.

Hot read-only accessors stay inline in `SceneRenderer.hpp`; heavier mutation and tile-rebuild code lives in `SceneRenderer.cpp` to keep the public header small without adding per-frame visibility access overhead.

## `WorldSceneIdentity`, transforms, hierarchy, renderables, and extraction

`SceneEntityId` is a persistent 128-bit `{high, low}` value; all-zero is invalid. It is distinct from the transient generational `World::Entity` handle. `WorldSceneIdentity` stores that ID and an optional UTF-8 display name. Names are bounded labels, may be empty or duplicated, and are never reference keys.

`generateWorldSceneEntityId(world)` creates an RFC 9562 UUIDv4 identity from the operating system CSPRNG (`getrandom`, `BCryptGenRandom`, or `arc4random_buf`). It applies the standard version/variant bits, checks the authoritative world state, and retries collisions up to a fixed 16-attempt bound. Entropy failure or collision exhaustion throws; the engine never falls back to a predictable generator. Linux generation may block only while the kernel initializes its entropy pool.

`setWorldSceneIdentity(world, entity, id, name)` is the normal authoring path. It requires a live entity, nonzero ID, valid NUL-free UTF-8, and ID uniqueness across the world before creating or replacing the component. Validation failure preserves the prior identity. `clearWorldSceneIdentity` removes an identity, while `findWorldSceneEntity` performs an authoritative allocation-free scan and returns the live transient handle; it detects duplicate-ID corruption instead of relying on a stale side index.

`WorldSceneTransform` stores the simulation-owned local-to-parent `TransformTRS`: translation, quaternion rotation, and scale. A transform without `WorldSceneParent` is a root and therefore world-space. `teleport(value)` updates the local pose and advances a discontinuity revision so presentation history resets instead of smearing across a discontinuous move. `WorldSceneParent` stores a generational parent entity handle; parents need not be renderable or have a transform. A transformless hierarchy node contributes identity locally while still inheriting its own parent. `WorldSceneRenderable` stores a mesh id, shader material, local `MeshBounds`, and visibility.

`setWorldSceneParent(world, child, parent)` is the normal hierarchy authoring path. It requires live endpoints, rejects self-parenting, descendant cycles, and already-cyclic ancestor chains before mutation, then creates or updates the single parent component. Rejected reparenting preserves the prior relationship. `clearWorldSceneParent(world, child)` removes the link and reports whether one existed; dead children return false.

`WorldSceneExtractor` owns reusable previous/current pose history keyed by entity index and validates every slot against the full generational entity handle and one observed `World` instance token. `prepareSimulationStep(world)` initializes new, recycled, or discontinuous entities before a fixed update; `captureSimulationStep(world)` shifts and captures poses after a successful update. A transform component that disappears for a completed step and is later re-added starts with identical history endpoints rather than reusing the prior component lifetime. `invalidateSimulationState()` clears prepared history after an aborted update, while `resetSimulationState(world)` snaps all endpoints to current poses explicitly. Newly spawned and recycled entities start with identical history endpoints, destroyed entities disappear through the normal ECS join, and history vectors retain capacity across frames.
For scheduler-backed updates, `Application` calls prepare, executes the complete compiled system plan plus its end-of-step structural playback, then captures. A failure skips capture and invalidates history, so presentation never blends against a partially scheduled step.

`build(world, interpolationAlpha)` clamps the presentation alpha, interpolates every ancestor's local translation/scale and normalized shortest-path quaternion rotation at that same alpha, then resolves each submitted model as `parentWorld * local`. Matrix composition preserves shear introduced by non-uniform parent scale and rotated children; no lossy world-TRS decomposition occurs. The same resolved matrix drives submitted geometry and world-space bounds. Radius scaling uses the Frobenius norm of the matrix's linear 3x3 portion, conservatively covering non-uniform scale and shear. Non-finite poses, non-finite resolved matrices, and invalid bounds are omitted.

Parent links are still validated during every extraction because ECS component fields remain directly mutable and manually assembled worlds can bypass the safe authoring functions. A dead or generation-stale parent detaches deterministically: the child survives and its local transform becomes a root transform, so recycling the parent's entity index cannot reattach the stale link. The iterative tri-color resolver cannot overflow the native stack; self-links, cycles, and every renderable depending on them are omitted without throwing, while unrelated entities continue rendering. Per-entity resolution and path storage retain capacity across builds.

The alpha is retained fixed-step debt divided by the step interval, so presentation intentionally interpolates from the penultimate state to the latest completed state rather than extrapolating an unknown future state. Extraction remains deterministic by sorting pending records by `{Entity::index, Entity::generation}` before populating its reusable `SceneRenderList`; the renderer borrows that list synchronously.

## World-scene persistence

`encodeWorldScene(world, limits)` and `decodeWorldScene(destination, bytes, limits)` persist the explicit scene bridge—not arbitrary ECS component pools. Version 2 covers `WorldSceneIdentity`, `WorldSceneTransform::current`, `WorldSceneParent`, and `WorldSceneRenderable`; presentation-only discontinuity revisions and gameplay components remain intentionally excluded. Every persisted v2 record requires a unique stable identity. Default limits bound input/output to 1,000,000 entities, 256 MiB, 255 bytes per name, and 16 MiB of aggregate name data.

The `VESN` binary stream uses explicit little-endian fields rather than native struct layouts. Its header is the magic, version, and entity count. Each v2 record stores the 128-bit ID, bounded UTF-8 name, component mask, and component payload. Records sort lexicographically by stable ID, and hierarchy references store parent stable IDs; runtime entity indices, generations, pointers, and handles never enter the file. Alive referenced parents with no transform or renderable remain valid identity-only records.

Version 1 streams remain loadable. Their file-local ordinal `i` migrates deterministically to `{0, i + 1}` with an empty name, existing ordinal parent links are remapped, and the next save emits v2. This preserves compatibility without pretending the old transient ordering was a cross-save identity.

Encoding rejects missing/zero/duplicate identities, malformed or oversized names, dangling parents, cycles, non-finite values, invalid mesh identifiers, and valid bounds with negative radius. Decoding additionally rejects bad magic/version, unknown component bits, invalid booleans, truncated or trailing bytes, malformed or unknown parent IDs, unreferenced identity-only records, and configured limit breaches. Minimum record footprints and name totals are checked before large allocations. Parsing and graph validation finish before a temporary `World` is constructed, and the destination is move-replaced only after the complete load succeeds.

`saveWorldScene(world, path, limits)` publishes encoded bytes through `writeBinaryFileAtomic`; `loadWorldScene(destination, path, limits)` reads and transactionally decodes them. The one-shot path uses bounded hash deduplication while gathering, canonical $O(N \log N)$ sorting, binary-search parent resolution, and linear encoding/reconstruction. It adds no per-frame processing.

## Reflected scene types

`SceneTypeRegistry` owns immutable component metadata keyed by stable 64-bit
FNV-1a type/property IDs. A `SceneTypeMetadata` record contains the display
name, stable name, schema version, payload size/alignment, inspector ranges,
and encode/decode/migrate/property-write hooks. Registration rejects duplicate
or malformed type/property IDs before publishing a type.

The built-in `Transform` and `Renderable` schemas are defined once in
`SceneComponents.annotations.hpp`. The build generator emits
`SceneSchema.generated.hpp`; `generatedSceneTypeRegistry()` and the handwritten
`explicitSceneTypeRegistry()` must produce byte-identical binding manifests.
`externalSceneTypeRegistry()` parses the line-oriented
`schemas/scene-components.schema` representation and produces that same
manifest. This keeps annotation, explicit-registration, and external-schema
prototypes executable while the long-term binding source remains evolvable.

Known payloads are canonical little-endian records. Transform schema v2 stores
translation, quaternion rotation, and scale; the registered v1-to-v2 migration
adds identity rotation to the legacy translation/scale payload. Decode and
migration validate the complete candidate before replacing caller state.

## Authoring documents and commands

Editor builds expose `editor::AuthoringDocument`. Entities use stable
`SceneEntityId`s, names, parent identities, and a sorted set of versioned
component payloads. Known components are decoded through the registry on every
load/edit; unknown component payloads and versions round-trip opaquely so a
tool does not destroy data it cannot inspect. Cooking rejects unknown types
instead of silently omitting them.

Create, subtree delete, rename, reparent, component replacement, reflected
property edits, and multi-entity edits are `DocumentCommand`s containing
before/after entity patches. A command applies to a copied candidate, validates
the full identity/component/hierarchy graph, then swaps atomically. Undo and
redo require the exact expected state; divergence raises without consuming the
history entry. History is bounded and grows lazily before publication, so
allocation failure cannot leave an applied edit without its inverse. Dirty
state compares the canonical document fingerprint with the last saved
fingerprint, so preview replacement, undo/redo, and transactional load all
report state rather than history position.

`encodeAuthoringDocument` writes canonical little-endian `VEAU` data sorted by
entity and component ID. `decodeAuthoringDocument` parses into a temporary
document, applies registered migrations, preserves unknown payloads, and
replaces the destination only after complete validation.
`saveAuthoringDocument` uses atomic publication; `importAuthoringScene`
deterministically maps a glTF scene's asset identities, hierarchy, transforms,
meshes, and materials into the same document model.

## Cooked worlds

`editor::cookAuthoringDocument` converts a validated document into compact
structure-of-arrays `CookedWorld` data: stable identities, names, parent
indices, transforms, renderable masks, and authored mesh/material identities.
Records sort by stable identity, parents resolve to indices, and all arrays
must have identical entity counts. `VECW` encode/decode and file helpers use
explicit little-endian fields, configured entity/name/byte limits, and
transactional replacement.

`instantiateCookedWorld(destination, source, resolver)` resolves authored asset
IDs into current generational runtime handles, renderer materials, and mesh
bounds while constructing a temporary `World`. Invalid/stale resolution,
non-finite data, malformed hierarchy, or any component failure leaves the
destination untouched. `Application::instantiateCookedWorld` supplies the
active renderer-backed resolver. Runtime builds include `CookedWorld` but not
the editor document, command stack, importer, cooker, session, or ImGui shell.

VESN v2 remains the explicit low-level `WorldScene*` snapshot contract described
above; creator workflows use `VEAU` authoring source and `VECW` runtime output
instead of treating VESN as a generic editor format.

## Streamed world partitions

`WorldPartitionManifest` is a canonical versioned hierarchy of stable cell
identities, parent links, bounds, split distances, artifact paths, byte
estimates, and runtime dependencies. Validation bounds cell/entity/byte totals,
requires finite positive geometry, rejects duplicate IDs, missing parents,
cycles, escaping artifact paths, sibling coverage gaps/overlap, and invalid
parent-child containment. Canonical encoding sorts identities; decoding rebases
validated relative artifact paths under the manifest directory.

`WorldPartitionRuntime` selects a deterministic leaf frontier from global
observer coordinates. Split thresholds use distance to cell bounds; a separate
prefetch margin requests upcoming children before activation. The active
frontier stays pinned and renderable while replacement artifacts load.
Publication combines complete `VECW` cells into a temporary `CookedWorld`,
rejects duplicate stable entities and unresolved cross-cell parents, rebases
translations around a quantized local origin, and exposes a revisioned
candidate. Only `commitPublication(revision)` changes active cells/origin;
stale commit and failed combination leave the prior frontier unchanged.

Rejected candidates retry explicitly. Retry evicts only candidate cells that
are not shared by the active frontier, so a failed partial transition cannot
discard retained coverage. Superseded cells are unpinned and evicted only after
the caller publishes the complete combined world. Metrics report traversal,
publication, origin-shift, retained-frontier, partial-failure, active/desired,
pending, and coverage-gap state.

## Deterministic landscape cooking

`LandscapeField` is an immutable-seed procedural query with a bounded,
revisioned brush list. `sample(worldX, worldZ)` returns finite height, normal,
moisture, temperature, and `LandscapeBiome`; `height()` is the cheaper scalar
query used by collision/camera placement. Invalid configuration, coordinates,
or brushes fail before mutation.

`cookTerrainPatch(field, desc)` produces a local-space indexed patch with
`resolution + 1` vertices per side, deterministic normals/UVs, explicit LOD
metadata, and skirts on all four boundaries. Resolution is power-of-two and
bounded; callers place the local mesh at `desc.center`, which makes patches
compatible with partition origin rebasing. `createWaterPatch()` produces a
bounded reusable plane at an explicit level.

`scatterFoliage(field, desc)` hashes integer candidate cells from seed and
coordinates, then applies deterministic density, biome, slope, spacing, jitter,
and capacity rules. It returns authored world positions, yaw, scale, and
`Grass`/`Shrub`/`Tree` species; exceeding the declared instance capacity is an
error rather than silent truncation. `createGrassClusterMesh()`,
`createShrubMesh()`, and `createTreeMesh()` provide reusable bounded low-poly
geometry.

## `SceneGridRange`

Describes a contiguous grid in a `SceneRenderList`:

- `firstItem`
- `rows`
- `columns`
- `valid`

The grid is row-major: item index = `firstItem + row * columns + column`.

## `SceneGridTile`

Precomputed culling/homogeneity record:

- row/column begin/end bounds.
- aggregate `boundsCenter` and `boundsRadius` for tile frustum tests.
- `maxItemBoundsRadius` — conservative aggregate radius retained for tile metadata; sphere LOD selection remains per item.
- `itemCount`.
- fixed per-`RenderMaterialClass` histogram, built with the tile and reused by
  whole-tile visibility accounting without revisiting every item.
- `commonMesh` and `homogeneousMesh`.

The aggregate tile radius is for culling, not item LOD. Fully accepted homogeneous sphere tiles reuse the tile frustum result but still classify each sphere independently into high/medium/low mesh batches.

## `DemoSceneRenderer`

Sandbox-only scene builder used by `Application`; it is not owned or invoked by the renderer backend.
Public API:

- `kFixedItemCount = 7`.
- `requiredItemCount(rows, columns)` — returns `rows * columns + kFixedItemCount` with overflow checks.
- `validateMaterialGridDimensions(rows, columns)` — throws on invalid/overflowing dimensions.
- `setImportedModelBounds(bounds)` — accepts bounds marked `valid` with finite local-space center and a non-negative finite radius from loaded mesh data; invalid bounds are ignored and the last valid bounds remain active.
- `build(simulationElapsedSeconds, rows, columns, tileRows, tileColumns) -> const SceneRenderList&` — returns a reused render list driven by the caller's bounded simulation timeline.

Behavior:

- Rebuilds static grid/ground/imported-model layout only when dimensions, tile size, or imported-model bounds change.
- Rewrites only the animated foreground items on stable layouts.
- Keeps material-grid tile metadata cached for renderer visibility acceleration.
