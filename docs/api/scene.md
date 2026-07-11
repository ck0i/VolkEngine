# Scene API

Header: `engine/renderer/SceneRenderer.hpp`; sandbox implementation: `engine/renderer/SceneRenderer.cpp`.

The scene API provides renderer-facing data plus a CPU-only `WorldSceneExtractor` bridge. The bridge converts explicit world transform/renderable components into an extractor-owned, reusable `SceneRenderList`; Vulkan remains behind the renderer boundary.

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
- `flags.x`: ground-grid overlay enable.
- `flags.y`: integral `RenderMaterialClass` ABI value (`Standard`, `Masked`, `ClearCoat`, `Foliage`, `Skin`, `Hair`, `Cloth`, or `Emissive`).
- `flags.z`: alpha cutoff for the masked class, or class-response strength for the other specialized models.
- `flags.w`: normal-map strength in `[0, 4]`.

Texture availability is encoded separately from this vec4 by the three `TextureAssetHandle` roles (base color, normal, ORM). Invalid, non-integral material classes and negative class/normal parameters are rejected before renderer borrowing.

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
