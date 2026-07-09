# GPU resource registry API

Header: `engine/renderer/GpuResourceRegistry.hpp`.

`GpuResourceRegistry` is diagnostic accounting for long-lived renderer resources. It does not own GPU memory, Vulkan handles, or VMA allocations. Callers should treat returned IDs as opaque debug accounting handles, not resource ownership.

Engine-side usage points (non-exhaustive):

- `engine/renderer/vulkan/VulkanRenderer.FrameResources.cpp` — per-frame buffers and descriptor set resources.
- `engine/renderer/vulkan/VulkanRenderer.Meshes.cpp` — geometry buffers.
- `engine/renderer/vulkan/VulkanRenderer.Resources.cpp` — texture image accounting and shared buffer/image unregister helpers.
- `engine/renderer/vulkan/VulkanRenderer.Swapchain.cpp` — imported swapchain images and swapchain-dependent image tracking.
- `engine/renderer/vulkan/VulkanRenderer.Frame.cpp` — screenshot readback buffer accounting.

Callers do not own registry entries; IDs are renderer-internal accounting handles.

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
