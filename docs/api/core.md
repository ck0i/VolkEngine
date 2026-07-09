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
| `exposure` | `1.0f` | Positive tonemap exposure push constant. |
| `shaderHotReload` | `false` | Poll copied SPIR-V files and atomically swap rebuilt pipelines on success. |
| `indirectSceneDraws` | `true` | Request multi-draw indirect; backend falls back when features are missing. |
| `debugOverlay` | `true` | Enables Dear ImGui backend and overlay when compiled in. |
| `gpuTimestamps` | `true` | Requests timestamp queries; unsupported/disabled results keep `gpuTimestampsValid = false`. |
| `materialGridRows`, `materialGridColumns` | `4`, `5` | Demo material-grid dimensions. |
| `materialGridTileRows`, `materialGridTileColumns` | `16`, `16` | Demo grid tile dimensions for culling acceleration. |
| `depthPrepassMode` | `DepthPrepassMode::ForceOff` | Explicit prepass switch; no adaptive mode yet. |
| `shaderDirectory`, `assetDirectory`, `cacheDirectory` | CMake-defined build paths | Runtime shader, asset, and pipeline-cache locations. |

## `RunOptions`

Execution controls used by `Application::run()`.

- `maxFrames`: `0` runs until close; nonzero exits after that many frames.
- `resizeSmoke`: triggers the built-in resize smoke path.
- `screenshotPath`: non-empty path requests a one-shot PPM screenshot.

## `DepthPrepassMode`

- `ForceOff`: default depth-writing HDR scene pass.
- `ForceOn`: depth-only prepass followed by HDR scene pass reading depth.

## `Application`

```cpp
ve::EngineConfig config{};
config.validation = true;
ve::Application app{config};
return app.run(ve::RunOptions{.maxFrames = 120});
```

`Application` owns the main `Window`, `Camera`, concrete `VulkanRenderer` facade, and `Clock`. It is the intended high-level entry point for the sandbox-style app loop and calls only the public renderer operations; private split internals stay behind `VulkanRenderer::Impl`.

## `Camera`

Mutable camera used by the window input layer and renderer.

- `setAspect(float)`: update projection aspect ratio.
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

It uses `std::chrono::steady_clock` and increments the frame index per tick.

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
