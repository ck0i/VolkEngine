# Core API

Primary headers: `engine/core/{Application,Config,JobSystem,World,WorldScheduler,Time,Camera,FileSystem,Log,Math}.hpp` and `engine/streaming/ResidencyManager.hpp`.

Use headers for signatures and defaults. The contracts below are the parts callers must preserve.

## Application

`Application` owns the window, camera, job system, active assets, Vulkan renderer, scene extractors, and clocks. It supports the sandbox scene, a caller-owned `World`, callback-based fixed updates, input-aware updates, and a compiled `WorldSystemScheduler`.

Fixed updates receive accumulated input edges and motion once on the first available substep; held state persists. Render-rate camera movement uses bounded wall delta. World extraction interpolates the two latest successful simulation states.

Integration hooks are non-owning:

- asset augmentation runs after reference cooking and before renderer creation;
- frame update runs before fixed-step simulation and may report a complete world replacement;
- renderer overlay runs inside an active ImGui frame when available;
- callback contexts must outlive construction or `run()` as appropriate.

`instantiateCookedWorld` resolves current asset handles into a temporary world and replaces the destination only on success. Streaming and landscape summary methods own bounded diagnostic samples, not runtime world state.

## Configuration

`EngineConfig` controls window size, validation, vsync, exposure, shader/asset reload, indirect draws, shadows, diagnostics, timestamps, simulation step/catch-up limits, job capacities, scene-grid dimensions, depth-prepass mode, and runtime directories. Invalid dimensions, exposure, simulation limits, or escaping relative asset paths fail before backend work.

`DepthPrepassMode` values are `Auto`, `ForceOff`, and `ForceOn`. `RunOptions` controls frame count, resize and acquire-recovery smoke paths, and screenshot output.

## Job system

`JobSystem` preallocates job slots, dependency edges, worker queues, and timeline storage. Ready work is local-LIFO and stolen FIFO. A generational `JobHandle` remains valid until explicit `release`; callbacks and contexts remain caller-owned until terminal completion.

Dependencies run only after every parent succeeds. Failure and cancellation propagate. Worker-side `wait` executes other ready work, which allows nested jobs to progress with one worker. Capacity exhaustion, stale handles, invalid dependencies, recursive release, and submission after shutdown fail rather than silently dropping work.

Shutdown may drain or cancel. Statistics expose state counts, queue high-water mark, steals, worker time, and General/Simulation/IO/Asset categories.

## World and scheduler

`World` is a generational entity registry with sparse-set component pools. Destroy or `clear` invalidates entity handles. Component removal uses swap-and-pop and may invalidate component references.

Structural mutation is forbidden during `each` queries. Field mutation and nested reads are allowed. `WorldCommandBuffer` records owned FIFO destroy/remove/emplace operations for later playback; dead or recycled targets are rejected without affecting replacement entities.

`WorldSystemScheduler::compile` performs a stable topological sort. Mutable systems execute serially. Dependency-independent read-only systems may share a parallel phase. A step failure joins submitted work, discards deferred commands, rolls back scheduler resources, and invalidates presentation history.

Typed event channels use fixed inline double buffers. Systems read the previous successful step and publish the next. Timer queues advance by successful simulation steps, not wall time. Event/timer overflow aborts before command playback; scheduling and publication allocate nothing after setup.

## Runtime residency

`ResidencyManager` uses the shared job system for texture, geometry, world-cell, animation, and audio artifacts. Per frame:

```text
beginFrame(frame) → request(key, priority, pin) ... → endFrame()
```

Requests expand dependencies. The desired set cancels obsolete generations; pinned dependencies resist eviction. Complete bytes publish in stable priority/identity order. Unpinned least-recently-used entries are evicted to satisfy the byte budget. Missing dependencies, cycles, IO failure, oversized artifacts, admission failure, backpressure, and cancellation remain distinct states.

## Time and camera

`Clock` uses `steady_clock`; `tickAt` supports deterministic tests and rejects backward samples without changing state. `FixedStepClock` caps retained time and emitted substeps and reports dropped time plus interpolation alpha.

`Camera` validates finite movement/rotation inputs and positive aspect ratio before mutation. Projection is reverse-Z in renderer use.

## File and utility functions

- `readBinaryFile(path[, maximumBytes])` checks bounded size before allocation.
- `writeBinaryFileAtomic(path, bytes)` writes a temporary sibling and publishes only after successful flush/close.
- `readTextFile(path)` returns file contents or throws with path context.
- `VE_CHECK` is a throwing runtime check, not a compiled-out assertion.

The math layer provides `Vec2/3/4`, column-major `Mat4`, basic vector/matrix operations, transforms, perspective, and look-at construction. It is intentionally smaller than a general math library.
