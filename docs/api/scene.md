# Scene API

Primary headers: `engine/renderer/SceneRenderer.hpp`, `engine/renderer/Lighting.hpp`, `engine/scene/SceneReflection.hpp`, `engine/scene/CookedWorld.hpp`, `engine/scene/WorldPartition.hpp`, and `engine/landscape/Landscape.hpp`.

## Renderer-facing scene data

`SceneRenderItem` contains a runtime mesh handle, model matrix, packed `RenderMaterial`, local bounds, and optional authored identity. The renderer borrows items synchronously; it does not retain the list.

`SceneRenderList` owns reusable item storage plus directional light, local lights, environment, reflection probes, and optional grid acceleration metadata. Replacement setters validate all records before changing the list. `clear()` removes items/lights/probes and restores default environment state.

`RenderMaterial` is a vec4-packed CPU/GLSL ABI. Texture presence is represented by mask bits, not inferred from indices. Material class, class strength, alpha cutoff, and normal strength occupy the documented packed fields. Keep CPU definitions and shader declarations in sync.

Lighting limits and ABI types live in `Lighting.hpp`. Forward+ uses 16×16 tiles, 64 local-light indices per tile, and at most 256 local lights. The shadow atlas has sixteen 512² slots; three are reserved for directional cascades. CPU reference builders are used by tests and must match GPU ordering and overflow behavior.

## World extraction

Runtime `World` entities use transient generational handles. `SceneEntityId` is a persistent 128-bit identity used by files and authoring data; zero is invalid. `WorldSceneIdentity` also stores a bounded display name, which is never a reference key.

`WorldSceneTransform` stores local-to-parent TRS. `WorldSceneParent` references a runtime entity. `WorldSceneRenderable` stores the authored mesh/material identity required for resolution.

`WorldSceneExtractor` owns reusable pose history and output storage. Before each successful fixed step:

```text
prepareSimulationStep(world)
execute systems and deferred commands
captureSimulationStep(world)
```

`build(world, alpha)` interpolates local translation/scale and shortest-path quaternion rotation, resolves ancestors parent-first, and emits stable entity order. New, recycled, re-added, or teleported transforms start with identical history endpoints. Dead parents detach; cyclic dependents are omitted. A failed step invalidates history instead of blending partial state.

## Persistence and reflection

`VESN` persists the built-in world-scene bridge, not arbitrary ECS pools. Version 2 stores stable identity, name, transform, hierarchy, and renderable payloads in canonical little-endian order. Runtime entity indices and pointers never enter the file. Decode validates size limits, UTF-8, unique IDs, component payloads, and complete acyclic hierarchy into temporary state before replacing the destination.

`SceneTypeRegistry` maps stable type/property IDs to immutable metadata, version, size/alignment, property ranges, and encode/decode/migration hooks. Registration validates a complete candidate before publication. Runtime reflection does not depend on ImGui or Vulkan.

## Authoring and cooked worlds

Editor builds expose `AuthoringDocument`, which owns stable entities, hierarchy, known or opaque versioned component payloads, selection, dirty fingerprint, and bounded command history. Commands cover entity lifecycle, naming, parenting, components, and property edits. Undo/redo verifies expected state and does not consume an entry on divergence.

`VEAU` is the canonical authoring format and preserves unknown payloads. Cooking rejects unknown runtime data and produces `VECW` structure-of-arrays content: identities, names, parents, transforms, and authored mesh/material IDs.

`instantiateCookedWorld(destination, source, resolver)` resolves current generational asset handles and builds a temporary runtime world. Any invalid record or stale/missing asset leaves `destination` unchanged.

## World partition

`WorldPartitionManifest` is a versioned hierarchy of stable cell IDs, bounds, split distances, artifact paths, byte estimates, and dependencies. Validation checks roots, parent/child consistency, containment, coverage, cycles, duplicate IDs/paths, and configured limits.

`WorldPartitionRuntime` chooses a leaf frontier from observer distance to bounds, requests upcoming children with a prefetch margin, and keeps the active frontier pinned until a complete candidate is available. Candidate cells combine into one `CookedWorld`; cross-cell identities and parents are validated before publication. Failed or stale candidates retain the active world. Origin shifts are quantized and applied to root transforms only.

Residency owns bytes and IO state. Partition runtime owns selection and candidate state. The application owns publication into the live `World`.

## Landscape cooking

`LandscapeField` provides deterministic height, normal, moisture, temperature, and biome queries from a seed plus a revisioned brush list. Invalid configuration, non-finite coordinates, and brush-capacity overflow fail.

`cookTerrainPatch` creates a local-space indexed grid with power-of-two resolution, LOD metadata, and skirts on all four edges. `scatterFoliage` applies seeded density, biome, slope, spacing, jitter, and capacity rules and emits grass/shrub/tree transforms. Water and reusable vegetation mesh builders feed the same imported mesh/material path as authored content.

## Demo scene

`DemoSceneRenderer` builds the sandbox grid and is not part of backend ownership. `SceneGridRange` and `SceneGridTile` describe contiguous grid items and precomputed culling/class histograms used by CPU visibility acceleration.
