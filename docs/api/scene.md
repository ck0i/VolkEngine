# Scene API

Header: `engine/renderer/SceneRenderer.hpp`; sandbox implementation: `engine/renderer/SceneRenderer.cpp`.

The current scene API is renderer-facing data, not an ECS. It exists to feed the Vulkan backend compact mesh/material/bounds records and to exercise batching/culling in the sandbox.

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

A reusable vector-backed render-list plus optional material-grid metadata.

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
- `maxItemBoundsRadius` for conservative sphere LOD when a homogeneous tile is accepted as one work record.
- `itemCount`.
- `commonMesh` and `homogeneousMesh`.

The aggregate tile radius is for culling, not item LOD.

## `DemoSceneRenderer`

Sandbox scene builder used by `VulkanRenderer`.

Public API:

- `kFixedItemCount = 7`.
- `requiredItemCount(rows, columns)` — returns `rows * columns + kFixedItemCount` with overflow checks.
- `validateMaterialGridDimensions(rows, columns)` — throws on invalid/overflowing dimensions.
- `setImportedModelBounds(bounds)` — supplies local-space imported-model bounds from loaded mesh data and invalidates the cached static layout.
- `build(elapsedSeconds, rows, columns, tileRows, tileColumns) -> const SceneRenderList&` — returns a reused render list.

Behavior:

- Rebuilds static grid/ground/imported-model layout only when dimensions, tile size, or imported-model bounds change.
- Rewrites only the animated foreground items on stable layouts.
- Keeps material-grid tile metadata cached for renderer visibility acceleration.
