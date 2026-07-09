# GPU resource registry API

Header: `engine/renderer/GpuResourceRegistry.hpp`.

`GpuResourceRegistry` is diagnostic accounting for long-lived renderer resources. It does not own GPU memory, Vulkan handles, or VMA allocations.

## Capacity and names

- `kMaxResources = 128`
- `kMaxNameLength = 64`
- `kInvalidId = 0xffffffffU`

Names are copied into fixed-size storage. Empty names become `"Unnamed GPU Resource"`.

## `GpuResourceKind`

- `Buffer`
- `Image`

## `Record`

Internal fixed-size record:

- `id`
- `name`
- `kind`
- `bytes`
- `imported`
- `live`

A resource can be renderer-owned or imported. Swapchain images are the current imported-image use case.

## `registerResource`

```cpp
std::uint32_t id = registry.registerResource(
    ve::GpuResourceKind::Buffer,
    "Scene Instance Buffer",
    byteSize,
    false);
```

Returns a stable ID until unregistered. Throws when capacity is exhausted.

## `unregisterResource`

```cpp
registry.unregisterResource(id);
```

- Ignores `kInvalidId`.
- Clears the matching live record if found.
- Silently ignores unknown IDs.

## `stats()`

Returns aggregate live-resource accounting:

- `liveResources`
- `buffers`
- `images`
- `importedImages`
- `bytes`
- `bufferBytes`
- `imageBytes`
- `importedImageBytes`
- `ownedImageBytes`

These are renderer estimates for objects the registry knows about. They intentionally exclude Dear ImGui backend internals, VMA allocator internals, transient staging resources, and total process/GPU residency.
