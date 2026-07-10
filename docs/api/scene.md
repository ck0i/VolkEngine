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
- `flags.x`: ground grid overlay enabled.
- `flags.y`: albedo/ORM material texture sampling enabled.
- `flags.z`: normal-map sampling enabled.
- `flags.w`: normal-map strength in `[0, 1]`.

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
List operations:

- `clear()` — clears items and invalidates grid metadata.
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

## `WorldSceneTransform`, `WorldSceneParent`, `WorldSceneRenderable`, and `WorldSceneExtractor`

`WorldSceneTransform` stores the simulation-owned local-to-parent `TransformTRS`: translation, quaternion rotation, and scale. A transform without `WorldSceneParent` is a root and therefore world-space. `teleport(value)` updates the local pose and advances a discontinuity revision so presentation history resets instead of smearing across a discontinuous move. `WorldSceneParent` stores a generational parent entity handle; parents need not be renderable or have a transform. A transformless hierarchy node contributes identity locally while still inheriting its own parent. `WorldSceneRenderable` stores a mesh id, shader material, local `MeshBounds`, and visibility.

`WorldSceneExtractor` owns reusable previous/current pose history keyed by entity index and validates every slot against the full generational entity handle and one observed `World` instance token. `prepareSimulationStep(world)` initializes new, recycled, or discontinuous entities before a fixed update; `captureSimulationStep(world)` shifts and captures poses after a successful update. A transform component that disappears for a completed step and is later re-added starts with identical history endpoints rather than reusing the prior component lifetime. `invalidateSimulationState()` clears prepared history after an aborted update, while `resetSimulationState(world)` snaps all endpoints to current poses explicitly. Newly spawned and recycled entities start with identical history endpoints, destroyed entities disappear through the normal ECS join, and history vectors retain capacity across frames.
For scheduler-backed updates, `Application` calls prepare, executes the complete compiled system plan plus its end-of-step structural playback, then captures. A failure skips capture and invalidates history, so presentation never blends against a partially scheduled step.

`build(world, interpolationAlpha)` clamps the presentation alpha, interpolates every ancestor's local translation/scale and normalized shortest-path quaternion rotation at that same alpha, then resolves each submitted model as `parentWorld * local`. Matrix composition preserves shear introduced by non-uniform parent scale and rotated children; no lossy world-TRS decomposition occurs. The same resolved matrix drives submitted geometry and world-space bounds. Radius scaling uses the Frobenius norm of the matrix's linear 3x3 portion, conservatively covering non-uniform scale and shear. Non-finite poses, non-finite resolved matrices, and invalid bounds are omitted.

Parent links are validated during every extraction because ECS component fields remain directly mutable. A dead or generation-stale parent detaches deterministically: the child survives and its local transform becomes a root transform, so recycling the parent's entity index cannot reattach the stale link. The iterative tri-color resolver cannot overflow the native stack; self-links, cycles, and every renderable depending on them are omitted without throwing, while unrelated entities continue rendering. Per-entity resolution and path storage retain capacity across builds.

The alpha is retained fixed-step debt divided by the step interval, so presentation intentionally interpolates from the penultimate state to the latest completed state rather than extrapolating an unknown future state. Extraction remains deterministic by sorting pending records by `{Entity::index, Entity::generation}` before populating its reusable `SceneRenderList`; the renderer borrows that list synchronously.

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
