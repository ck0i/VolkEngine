# Core API

Headers: `engine/core/Config.hpp`, `Application.hpp`, `Camera.hpp`, `Time.hpp`, `FileSystem.hpp`, `Log.hpp`, `Math.hpp`, `Assert.hpp`.

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
| `indirectSceneDraws` | `true` | Request multi-draw indirect; backend falls back when features are missing. |
| `debugOverlay` | `true` | Enables Dear ImGui backend and overlay when compiled in. |
| `gpuTimestamps` | `true` | Requests timestamp queries; unsupported/disabled results keep `gpuTimestampsValid = false`. |
| `fixedSimulationStepSeconds` | `1 / 60` | Finite positive gameplay/world update interval. |
| `maximumSimulationAccumulatedSeconds` | `0.25` | Finite backlog cap, at least one fixed step; excess wall time is dropped to prevent unbounded catch-up. |
| `maximumSimulationSubsteps` | `8` | Positive per-render-frame update budget that bounds simulation CPU work while retaining bounded debt. |
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
`Application` owns the main `Window`, `Camera`, concrete `VulkanRenderer` facade, demo/world scene extractors, wall `Clock`, and `FixedStepClock`. It builds the per-frame scene submission and passes it to the renderer; the backend only borrows that list during `draw()`. Private split internals stay behind `VulkanRenderer::Impl`.
`run(options)` retains the sandbox's `DemoSceneRenderer` path, including material-grid metadata. `run(world, options)` renders a caller-prepared world without mutating it. `run(world, update, options)` invokes the legacy non-owning function-pointer callback zero or more times per rendered frame using the configured fixed step. `runWithInput(world, update, options)` additionally passes a read-only `InputState` snapshot to each substep. Input transitions and accumulated cursor/scroll motion are retained across render frames that emit no simulation update, delivered once to the first available substep, then consumed; held state persists for later substeps. Camera input remains render-rate and uses the bounded wall delta. Both world callback paths then extract the latest world snapshot and submit it synchronously. The caller owns `World`, must keep it alive for the full call, and must not mutate it concurrently.

### `WorldSystemScheduler`

`WorldSystemScheduler` replaces a monolithic world callback with a compiled, deterministic fixed-step system plan. Systems register an owned, case-sensitive name, a non-owning callback/context pair, and owned dependency names. Registration rejects empty/duplicate names, null callbacks, and malformed duplicate dependencies without changing the registry.

`compile()` resolves dependencies and performs a stable topological sort: every named dependency executes first, and otherwise-ready systems use registration index as the tie-break. Missing dependencies throw `std::invalid_argument`; cycles throw `std::runtime_error`. Either failure leaves no published plan. Registry mutation invalidates a prior plan, and `execute()` rejects uncompiled or recursive use.

Each callback receives `(context, world, commands, input, simulationElapsedSeconds, simulationDeltaSeconds)`. Systems run single-threaded in compiled order. The restricted `CommandWriter` records `destroy`, `remove<T>`, and owned `emplace<T>` operations but cannot trigger playback or discard work mid-step. The scheduler plays its buffer once after all systems in the fixed step, so structural changes become visible only at that boundary. A system/playback exception propagates and discards scheduler-owned deferred work for the failed step. `reserveSystems()` and `reserveDeferredCommandSlots()` move registry and command-vector growth out of steady execution; callback work and type-erased component payload storage may still allocate. With no allocating callback or command capture, compiled ordering and dispatch allocate nothing.
The `CommandWriter&` is valid only for the callback invocation and is non-copyable/non-movable; systems must not retain its address.

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
