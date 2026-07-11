# Core API

Headers: `engine/core/Config.hpp`, `Application.hpp`, `JobSystem.hpp`, `WorldScheduler.hpp`, `Camera.hpp`, `Time.hpp`, `FileSystem.hpp`, `Log.hpp`, `Math.hpp`, `Assert.hpp`.

## `EngineConfig`

Runtime renderer/application configuration. Construct it on the stack, override fields, then pass it to `Application` or backend integration code.

| Field | Default | Contract |
| --- | --- | --- |
| `applicationName` | `"VolkEngine Sandbox"` | Window/application label. |
| `initialWidth`, `initialHeight` | `1280`, `720` | Validated by the sandbox before crossing GLFW's `int` size API. |
| `validation` | build-dependent `VOLKENGINE_VALIDATION != 0` | Requests validation layers; startup still depends on system layer availability. |
| `vsync` | `true` | `true` selects FIFO; `false` asks for immediate-first fallback. |
| `exposure` | `1.0f` | Positive finite tonemap exposure; `VulkanRenderer` rejects invalid programmatic configs before backend startup. |
| `shaderHotReload` | `false` | Poll copied SPIR-V files and atomically swap rebuilt pipelines on success. |
| `assetHotReload` | `false` | Runs reference-asset source reads/import/cook work on the bounded job system and transactionally publishes a complete candidate at a main-thread frame boundary. |
| `indirectSceneDraws` | `true` | Request multi-draw indirect; backend falls back when features are missing. |
| `shadows` | `true` | Enables directional cascades and eligible local spot-shadow views. Disabling it preserves Forward+ direct/environment lighting while reporting shadow timing as unavailable. |
| `debugOverlay` | `true` | Enables Dear ImGui backend and overlay when compiled in. |
| `gpuTimestamps` | `true` | Requests timestamp queries; unsupported/disabled results keep `gpuTimestampsValid = false`. |
| `fixedSimulationStepSeconds` | `1 / 60` | Finite positive gameplay/world update interval. |
| `maximumSimulationAccumulatedSeconds` | `0.25` | Finite backlog cap, at least one fixed step; excess wall time is dropped to prevent unbounded catch-up. |
| `maximumSimulationSubsteps` | `8` | Positive per-render-frame update budget that bounds simulation CPU work while retaining bounded debt. |
| `jobWorkerCount` | `0` | Worker count; zero selects `hardware_concurrency - 1` with a minimum of one. |
| `maximumJobs`, `maximumJobDependencies`, `jobTimelineCapacity` | `4096`, `16384`, `8192` | Fixed scheduler slot, dependency-edge, and timeline capacities allocated during application construction. |
| `materialGridRows`, `materialGridColumns` | `4`, `5` | Demo material-grid dimensions. |
| `materialGridTileRows`, `materialGridTileColumns` | `16`, `16` | Demo grid tile dimensions for culling acceleration. |
| `depthPrepassMode` | `DepthPrepassMode::Auto` | Adaptive prepass selection by visible scene complexity; `ForceOn` and `ForceOff` are deterministic overrides. |
| `shaderDirectory`, `assetDirectory`, `cacheDirectory` | CMake-defined build paths | Runtime shader, asset, and pipeline-cache locations. |
| `groundAlbedoTexture`, `groundNormalTexture`, `groundOrmTexture` | `textures/ground_{albedo,normal,orm}.png` relative to `assetDirectory` | Role-specific ground material inputs. Relative paths are normalized and must remain inside `assetDirectory`; escaping paths resolve to empty and are rejected before texture upload. Absolute paths bypass the asset directory. |
| `importedModelPath` | `models/imported_showcase.obj` relative to `assetDirectory` | Wavefront OBJ imported as the `SceneMeshId::ImportedModel` batch. Relative paths are normalized and must remain inside `assetDirectory`; escaping paths resolve to empty and are rejected before mesh upload. Absolute paths bypass the asset directory. |

`isValidExposure(float)` is the shared helper for sandbox CLI parsing and programmatic config checks.

## `RunOptions`

Execution controls used by `Application::run()`.

- `maxFrames`: `0` runs until close; nonzero exits after that many frames.
- `resizeSmoke`: triggers the built-in resize smoke path.
- `acquireRecoverySmoke`: injects one post-acquire/pre-submit renderer failure; the renderer must recover internally and render subsequent frames. Intended for synchronization smoke testing.
- `screenshotPath`: non-empty path requests a one-shot PPM screenshot.

## `DepthPrepassMode`

- `Auto`: default mode; compiles a frame-graph superset and records the depth prepass only when visible item/triangle counts cross hysteresis thresholds.
- `ForceOff`: depth-writing HDR scene pass.
- `ForceOn`: depth-only prepass followed by HDR scene pass reading depth.

## `Application`

```cpp
ve::EngineConfig config{};
config.validation = true;
ve::Application app{config};
return app.run(ve::RunOptions{.maxFrames = 120});
```
`Application` owns the main `Window`, `Camera`, bounded `JobSystem`, active `ReferenceAssetBundle`, concrete `VulkanRenderer` facade, demo/world scene extractors, wall `Clock`, and `FixedStepClock`. Asset cooking starts before platform/renderer construction. With `assetHotReload` enabled, subsequent cooks execute in the background; the main thread only polls completion and publishes a validated candidate at a frame boundary. Publication replaces GPU geometry, clusters, textures, descriptors, authored draws, and visibility caches transactionally, while failure leaves the prior CPU/GPU bundle live.
`run(options)` retains the sandbox's `DemoSceneRenderer` path, including material-grid metadata. `run(world, options)` renders a caller-prepared world without mutating it. `run(world, update, options)` invokes the legacy non-owning function-pointer callback zero or more times per rendered frame using the configured fixed step. `runWithInput(world, update, options)` additionally passes a read-only `InputState` snapshot to each substep. Input transitions and accumulated cursor/scroll motion are retained across render frames that emit no simulation update, delivered once to the first available substep, then consumed; held state persists for later substeps. Camera input remains render-rate and uses the bounded wall delta. Both world callback paths then extract the latest world snapshot and submit it synchronously. The caller owns `World`, must keep it alive for the full call, and must not mutate it concurrently.

Creator/runtime integration uses three narrow application seams:

- `instantiateCookedWorld(destination, source)` resolves cooked authored
  mesh/material IDs through the active renderer and transactionally replaces
  `destination`; any stale asset or invalid cooked record leaves it unchanged.
- `setRendererOverlay(callback, context)` installs a non-owning ImGui overlay
  callback. When ImGui is compiled in, `EngineConfig::debugOverlay` is enabled,
  and backend initialization succeeds, the callback receives camera, renderer
  stats, and swapchain dimensions after `ImGui::NewFrame`; otherwise it is
  retained but not invoked.
  Its context must outlive the run.
- `jobStats()` returns the bounded scheduler aggregate used by profiling views.

These methods expose no Vulkan handles and do not make `Application` own an
editor document or shell.

### `JobSystem`

`JobSystem` is a fixed-capacity dependency scheduler. Construction preallocates job slots, dependency edges, per-worker queues, and a bounded timeline. External submissions distribute ready jobs round-robin; each worker consumes its own queue LIFO and steals from peers FIFO. `JobDesc` names are copied into fixed timeline storage, while callbacks and contexts remain non-owning and must outlive completion.

`submit()` returns a generational `JobHandle`. Dependencies become ready only after every parent succeeds; parent failure or cancellation propagates to dependents. `wait()` and `waitAll()` execute other ready work when called by a worker, so nested jobs remain live with one worker and do not consume a blocked thread. Running callbacks inspect `JobContext::cancellationRequested()` and may cooperatively `wait()` on children. Completed handles retain their terminal status and exception until explicit `release()`/`releaseAll()`; stale handles are rejected.

Capacity exhaustion, duplicate/stale dependencies, recursive terminal release, and submission after shutdown fail explicitly. Shutdown supports drain or cancellation. `stats()` snapshots submitted/succeeded/failed/cancelled/active/running counts, queue high-water mark, steals, total worker time, and per-category General/Simulation/IO/Asset counters. `timeline()` returns bounded completion records with queue/start/finish timestamps and worker assignment.

### `WorldSystemScheduler`

`WorldSystemScheduler` replaces a monolithic world callback with a compiled, deterministic fixed-step system plan. Systems register an owned, case-sensitive name, a non-owning callback/context pair, and owned dependency names. Registration rejects empty/duplicate names, null callbacks, and malformed duplicate dependencies without changing the registry.

`compile()` resolves dependencies and performs a stable topological sort: every named dependency executes first, and otherwise-ready systems use registration index as the tie-break. Missing dependencies throw `std::invalid_argument`; cycles throw `std::runtime_error`. Either failure leaves no published plan. Registry mutation invalidates a prior plan, and `execute()` rejects uncompiled or recursive use.

Mutable callbacks receive `(context, world, commands, input, simulationElapsedSeconds, simulationDeltaSeconds)` and execute serially in compiled order. `addParallelSystem()` instead registers a read-only callback over `const World` and `const InputState`; the compiler groups dependency-independent read-only systems into parallel phases and inserts a barrier before every mutable system or dependent phase. Parallel callbacks cannot obtain `CommandWriter`, so the existing mutable `World` plus deferred-command ownership contract remains serial.

The restricted `CommandWriter` records `destroy`, `remove<T>`, and owned `emplace<T>` operations but cannot trigger playback or discard work mid-step. Structural changes become visible only at the end-of-step playback boundary. Parallel failure still joins every submitted phase job, propagates the first exception, and rolls back commands plus scheduler-owned simulation resources. `reserveSystems()`, `reserveSimulationResources()`, and `reserveDeferredCommandSlots()` move scheduler growth out of steady execution; with non-allocating callbacks, compiled dispatch performs no allocations.

#### Typed simulation events

`createEventChannel<T, Capacity>()` creates a scheduler-owned `SimulationEventChannel` during setup and invalidates the compiled plan. Payloads are fixed-size, trivially copyable, nothrow-default-constructible values; both FIFO buffers live inline in the channel. Channel creation may allocate once inside scheduler ownership, but `publish`, `events`, checkpoint, rollback, and promotion allocate nothing. Returned references remain valid until scheduler destruction.

Systems read only `events()` from the previous successful step and publish into a separate pending buffer. Successful execution retires the current batch and promotes pending events in FIFO order, giving deterministic one-successful-step latency regardless of producer/consumer system order. A full channel latches overflow; the scheduler raises `std::overflow_error` before structural command playback instead of silently dropping the step.

Callback or overflow failure discards commands, removes events published after the step checkpoint, and retains the current batch for retry. Command playback is intentionally different because playback itself may have committed a prefix before throwing: remaining commands are discarded, while current events are consumed and newly published events are promoted so retry cannot duplicate events against an already-partially-mutated world. `reset()` clears both batches outside execution and rejects use during an active scheduler transaction.

#### Fixed-step simulation timers

`createTimerQueue<T, Capacity>()` creates a scheduler-owned `SimulationTimerQueue` with typed inline payload storage. `schedule(delaySteps, payload[, repeatSteps])` returns a monotonic `TimerHandle`; zero delay normalizes to one successful step, and a nonzero repeat interval reschedules the same handle. `cancel(handle)` stages removal of active or not-yet-promoted timers, while stale, invalid, and already-canceled handles return false. `reset()` clears timer state and returns the queue tick to zero without reusing issued handles.

Timers count successful scheduler executions rather than wall time or render frames. External and callback scheduling is staged, so a timer never expires in the step that schedules it. Due events are exposed as a deterministic FIFO `span<SimulationTimerEvent<T>>`, ordered by due tick and handle; the current due batch is immutable, so cancellation during its callback affects future repeats rather than erasing an event another system may already have observed.

Timer schedules and cancellations share the scheduler's resource transaction. Callback or resource-overflow failure rolls mutations back and retries the same due batch; partial command-playback failure promotes them alongside typed events. Capacity, tick, due-time, and handle exhaustion latch overflow and abort before command playback. Queue operations and successful-step advancement allocate nothing after scheduler setup.

`Application::run(world, scheduler, options)` requires a compiled scheduler before entering the platform loop. It retains the same accumulated input, fixed-step timing, transform-history prepare/capture, exception invalidation, and presentation interpolation used by callback-based world runs. Callback/context storage is non-owning and must remain alive through the run.

## `Camera`

Mutable camera used by the window input layer and renderer.

`setAspect(float)` updates the projection aspect ratio. It requires a finite, strictly positive value and throws before changing the previous aspect when given zero, a negative value, NaN, or infinity.
- `update(forward, right, up, yawDelta, pitchDelta, dt)`: applies movement and rotation deltas; all inputs must be finite and `dt` must be non-negative, otherwise it throws before changing camera state.
- `rotate(yawDegrees, pitchDegrees)`: applies finite angular deltas and rejects rotations that would make the stored angles non-finite.
- `viewMatrix()`, `projectionMatrix()`, `viewProjectionMatrix()`: derive matrices on demand.
- `position()`, `forward()`, `right()`: expose camera vectors.

Defaults: position `(0, 1.6, 5)`, yaw `-90°`, pitch `-12°`, vertical FOV `65°`, near `0.05`, far `500`.

## `Clock` and `FrameTiming`

`Clock::tick()` returns:

- `deltaSeconds`
- `elapsedSeconds`
- `frameIndex`

The default constructor anchors to `std::chrono::steady_clock::now()`. For deterministic simulation and tests, `Clock` also accepts an explicit steady-clock anchor and exposes `tickAt(time_point)`. The first sample at the anchor returns zero delta, zero elapsed time, and frame index zero; each accepted sample increments the frame index.

Samples must be nondecreasing. A timestamp earlier than the previous sample throws `std::runtime_error` before mutating clock state, so a rejected sample cannot poison elapsed time or frame indexing. Runtime `tick()` remains the production path and delegates to the same monotonic sampling logic.

`clampDeltaSeconds(deltaSeconds, maximumSeconds)` is a pure simulation helper. A finite positive delta passes through up to a finite positive maximum; negative, zero, NaN, or infinite inputs produce zero, while finite hitches cap at the maximum. This does not change the raw wall-clock timing reported by `Clock`, so telemetry and renderer frame metrics retain the original delta. `advanceSimulationSeconds(currentSeconds, deltaSeconds, maximumDeltaSeconds)` accumulates the bounded delta for gameplay and scene timelines, resets a non-finite current value to zero, and saturates finite addition overflow at `std::numeric_limits<double>::max()`.

`FixedStepClock` decouples gameplay/world updates from render cadence. It accumulates finite positive wall deltas, emits constant-duration `FixedStepBatch` updates, retains unconsumed debt across frames, caps both retained time and substeps per render frame, and reports dropped time plus a bounded interpolation alpha. Invalid construction parameters throw before platform startup. `reset()` clears elapsed and retained state.

## `World`

`World` is the runtime's generational entity/component registry. `createEntity()` returns an opaque `{index, generation}` handle; destroying an entity invalidates that handle before its slot can be recycled, and `clear()` invalidates every handle issued before the clear. Component pools use sparse lookup plus dense storage for constant-time access and cache-friendly iteration. Removing a component uses swap-and-pop, so component references may be invalidated by structural changes; entity handles remain stable for the entity lifetime. Moving a `World` preserves live entity handles.

- `emplace<T>(entity, args...)` constructs one component of type `T`; duplicate insertion throws.
- `tryGet<T>(entity)`, `contains<T>(entity)`, `remove<T>(entity)`, and `componentCount<T>()` provide component access.
- `instanceToken()` identifies the current `World` state lifetime. Construction, `clear()`, move replacement, and moved-from reset receive fresh monotonic tokens, allowing external caches to reject same-address and same-entity-generation history from an earlier world state.
- `reserveEntities(capacity)` reserves slot/free-index storage and rejects capacities beyond the 32-bit entity index range; `entityCapacity()` reports reserved slot capacity.
- `reserveComponents<T>(capacity)` reserves dense component/entity storage for `T`; `componentCapacity<T>()` reports the dense capacity without changing component semantics.
- `each<Ts...>(function)` iterates a one-or-more-component query as `(Entity, Ts&...)`; the `const World` overload supplies `(Entity, const Ts&...)` without permitting mutation. Component types must be distinct, unqualified object types.
- Multi-component queries probe the smallest dense pool, skip non-matches, and preserve callback argument order from the template parameter list. This keeps sparse-set joins bounded by the least-populated required component while maintaining a stable callback contract.
- Structural mutations (`createEntity`, `destroyEntity`, `clear`, `emplace`, `remove`, reservations, and move construction/assignment) throw `std::logic_error` while any `each` callback is active. Component field writes, nested queries, and const-query reads remain allowed; the guard is released when callbacks return or throw.
- `WorldCommandBuffer` records FIFO `destroy`, `remove<T>`, and `emplace<T>` operations for explicit playback after a query. Component insertion accepts an owned `T` value, so deferred commands never retain constructor arguments or caller references.
- `playback(world)` returns applied/rejected counts. Dead or recycled entity handles, missing removals, and duplicate insertions are rejected without mutating the replacement entity. Playback itself is forbidden while any query is active, including for an empty buffer.
- Playback detaches its current batch: commands recorded reentrantly during playback remain pending until the next call. If an operation throws, that attempted operation is consumed, the exception propagates, and the unattempted FIFO tail remains queued.
- `WorldSceneTransform` stores a simulation local-to-parent `TransformTRS`; roots use world-space values. `WorldSceneParent` links a child to a generational entity handle without adding hierarchy hooks to the generic ECS. `WorldSceneExtractor` retains previous/current local poses, interpolates ancestors with `FixedStepBatch::interpolationAlpha`, and resolves renderer-facing matrices parent-to-child. `teleport()` marks local discontinuities that must not interpolate. Dead/stale parents detach to roots, cyclic dependents are omitted, and extraction ordering remains deterministic by entity handle.
- `clear()` destroys all entities and component storage.

## File IO helpers

- `readBinaryFile(path[, maximumBytes]) -> std::vector<std::byte>`
- `writeBinaryFileAtomic(path, bytes)`
- `readTextFile(path) -> std::string`

Use these for engine file access so errors remain consistent across scene, shader, asset, and test code.
The bounded binary-read overload checks the on-disk length before allocating or reading, so hostile or accidental oversized files cannot bypass caller limits.

`writeBinaryFileAtomic` writes a uniquely named sibling temporary, verifies write/flush/close, and publishes only after completion. POSIX uses atomic rename replacement; Windows uses `MoveFileExW` with replacement and write-through flags. Failures leave the prior destination untouched and remove the handled temporary file.

## Logging

- `initializeLogging()` creates/configures the shared spdlog logger.
- `logger()` returns the shared `std::shared_ptr<spdlog::logger>&`.

## Math helpers

Small in-house math layer, intentionally limited until broader math needs are proven.

Types:

- `Vec2`
- `Vec3`
- `Vec4` (`alignas(16)`)
- `Mat4` (`alignas(16)`, column-major shader-friendly layout)

Functions/operators:

- vector add/subtract/multiply/divide
- `dot`, `cross`, `length`, `normalize`
- matrix multiply
- `translate`, `scale`, `rotateY`
- `perspective`, `lookAt`
- `radians`

## Assertions

`VE_CHECK(expr)` throws `std::runtime_error` with expression, file, and line through `failCheck()`. It is a runtime check, not a compiled-out assert.
