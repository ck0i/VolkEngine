# GPU resource registry

Header: `engine/renderer/GpuResourceRegistry.hpp`.

`GpuResourceRegistry` tracks diagnostic estimates for renderer-owned and imported buffers/images. It does not own GPU resources or control allocation.

## Contract

`registerResource(kind, name, bytes, imported)` copies the name and returns a monotonic opaque ID. An empty name is stored as `"Unnamed GPU Resource"`. `unregisterResource(id)` removes a matching live record and does nothing for an invalid or unknown ID.

The live record vector grows as needed; there is no fixed resource cap. Removed capacity may be reused, but IDs are never recycled.

`stats()` recomputes:

- live resource, buffer, and image counts;
- total estimated bytes;
- imported resource count and bytes.

Byte totals saturate at `std::uint64_t::max()` rather than wrapping. Because aggregates are recomputed from live records, removal restores the exact remaining total.

These numbers cover records the renderer registers. They are not VMA heap usage or process/device residency and exclude allocator internals, temporary staging allocations, and ImGui backend resources.
