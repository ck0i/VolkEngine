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
`Application` owns the main `Window`, `Camera`, concrete `VulkanRenderer` facade, demo/world scene extractors, and `Clock`. It builds the per-frame scene submission and passes it to the renderer; the backend only borrows that list during `draw()`. Private split internals stay behind `VulkanRenderer::Impl`.
`run(options)` retains the sandbox's `DemoSceneRenderer` path, including material-grid metadata. `run(world, options)` renders a caller-prepared world without mutating it. `run(world, update, options)` invokes the non-owning function-pointer callback once per frame after clamped simulation timing and camera input, then extracts the read-only world snapshot and submits it synchronously. The caller owns `World`, must keep it alive for the full call, and must not mutate it concurrently.

## `Camera`

Mutable camera used by the window input layer and renderer.

`setAspect(float)` updates the projection aspect ratio. It requires a finite, strictly positive value and throws before changing the previous aspect when given zero, a negative value, NaN, or infinity.
- `update(forward, right, up, yawDelta, pitchDelta, dt)`: apply movement and rotation deltas.
- `rotate(yawDegrees, pitchDegrees)`: apply angular deltas directly.
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

`clampDeltaSeconds(deltaSeconds, maximumSeconds)` is a pure simulation helper. It clamps negative, non-finite, or hitch-sized deltas to a non-negative maximum without changing the raw wall-clock timing reported by `Clock`; telemetry and renderer frame metrics retain the original delta. `advanceSimulationSeconds(currentSeconds, deltaSeconds, maximumDeltaSeconds)` accumulates this bounded delta for gameplay and scene simulation timelines.

## `World`

`World` is the runtime's generational entity/component registry. `createEntity()` returns an opaque `{index, generation}` handle; destroying an entity invalidates that handle before its slot can be recycled, and `clear()` invalidates every handle issued before the clear. Component pools use sparse lookup plus dense storage for constant-time access and cache-friendly iteration. Removing a component uses swap-and-pop, so component references may be invalidated by structural changes; entity handles remain stable for the entity lifetime. Moving a `World` preserves live entity handles.

- `emplace<T>(entity, args...)` constructs one component of type `T`; duplicate insertion throws.
- `tryGet<T>(entity)`, `contains<T>(entity)`, `remove<T>(entity)`, and `componentCount<T>()` provide component access.
- `reserveEntities(capacity)` reserves slot/free-index storage and rejects capacities beyond the 32-bit entity index range; `entityCapacity()` reports reserved slot capacity.
- `reserveComponents<T>(capacity)` reserves dense component/entity storage for `T`; `componentCapacity<T>()` reports the dense capacity without changing component semantics.
- `each<Ts...>(function)` iterates a one-or-more-component query as `(Entity, Ts&...)`; the `const World` overload supplies `(Entity, const Ts&...)` without permitting mutation. Component types must be distinct, unqualified object types.
- Multi-component queries probe the smallest dense pool, skip non-matches, and preserve callback argument order from the template parameter list. This keeps sparse-set joins bounded by the least-populated required component while maintaining a stable callback contract.
- Structural mutations (`createEntity`, `destroyEntity`, `clear`, `emplace`, `remove`, reservations, and move construction/assignment) throw `std::logic_error` while any `each` callback is active. Component field writes, nested queries, and const-query reads remain allowed; the guard is released when callbacks return or throw.
- `WorldSceneTransform` and `WorldSceneRenderable` are the explicit world-owned render extraction components. `WorldSceneExtractor` joins them into renderer-facing `SceneRenderItem` records; extraction ordering is deterministic by entity handle, independent of dense-pool swap-and-pop order.
- `clear()` destroys all entities and component storage.

## File IO helpers

- `readBinaryFile(path) -> std::vector<std::byte>`
- `readTextFile(path) -> std::string`

Use these for engine file reads so errors remain consistent across shader, asset, and test code.

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
