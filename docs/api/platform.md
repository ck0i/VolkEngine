# Platform API

Headers: `engine/platform/Window.hpp`, `engine/platform/Input.hpp`.

`GlfwRuntime` owns GLFW's process-global initialization. `Window` owns one `GLFWwindow*`, translates native callbacks into a zero-allocation `InputTracker`, tracks framebuffer resize state, and creates Vulkan surfaces for the backend. `InputState`, `CameraInput`, and their helpers remain GLFW-free for deterministic engine, gameplay, test, and replay code.

## Construction and lifetime

```cpp
ve::EngineConfig config{};
ve::GlfwRuntime glfwRuntime{};
ve::Window window{glfwRuntime, config};
```

- `GlfwRuntime` calls `glfwInit()` exactly once for its lifetime and throws if initialization fails.
- Only one `GlfwRuntime` may be active in a process; GLFW has one process-global instance and must be initialized and terminated on the main thread.
- `Window` requires a live `GlfwRuntime`, creates its GLFW window from `EngineConfig`, and destroys only that window.
- The runtime must outlive every `Window` that borrows it.
- `GlfwRuntime` and `Window` copy/move operations are deleted.

## Event loop

- `pollEvents()` — non-blocking GLFW event pump.
- `waitEvents()` — blocking event wait; used for minimized/zero-size framebuffer waits.
- `shouldClose() const` — true when the window should exit.
- `requestClose()` — asks the window to close.

## Input and camera

GLFW callbacks update `InputTracker` as events arrive, preserving keyboard and mouse presses/releases that both occur between render frames. `pollInput()` consumes one `InputState` value snapshot after polling events and four standard-gamepad slots. Gamepads use GLFW's mapping database and are sampled once per render frame, so transitions occurring entirely between polls are not observable.

The public snapshot is GLFW-free:

- `InputKey` covers letters, digits, navigation, modifiers, and function keys.
- `InputMouseButton` covers eight stable buttons.
- `GamepadButton` follows the standard face/bumper/thumb/D-pad layout.
- `GamepadAxis` exposes left/right sticks with engine-semantic positive Y upward and triggers normalized to `[0, 1]`.
- Four stable slots map to GLFW joystick slots 1-4. A disconnect neutralizes axes and releases formerly held buttons exactly once; a reconnect begins a fresh button lifetime.
- `GamepadDeadzone` defaults to radial `0.15` stick and `0.05` trigger thresholds. Values are continuously rescaled outside each threshold, stick diagonals retain direction, and published axes remain finite and bounded.

`InputState` exposes `pressed`, `held`, and `released` keyboard, mouse, and per-slot gamepad queries. Cursor and scroll deltas accumulate across all events in a render frame. Holding the right mouse button captures the cursor without a jump; focus loss releases keyboard/mouse inputs and reseeds the cursor baseline.

`Application` accumulates each render snapshot into its fixed-step input buffer. Transitions and pointer deltas survive render frames with no simulation step, appear on the first eventual step, and clear before additional catch-up steps. Held keyboard, mouse, and gamepad levels remain available to every step.

`updateCamera(Camera& camera, const InputState& input, float dt)` applies the same snapshot gameplay receives through `Application::runWithInput(...)`; the compatibility overload consumes the tracker directly. `mapCameraInput(...)` combines keyboard/mouse with slot 0: left stick moves horizontally, right stick looks, and the triggers move down/up. Combined signed axes clamp to `[-1, 1]`. `applyCameraInput(...)` rejects non-finite axes, negative/non-finite delta time, and camera-step overflow before committing a camera change.

Current sandbox controls:

- `Esc`: close.
- `WASD`: horizontal movement.
- `Q` / `E`: down/up movement.
- Arrow keys: look.
- Hold right mouse button: captured mouse-look.
- `Space` or gamepad 1 `A` with `--world-scene`: pause/resume cube rotation.
- Gamepad 1 left stick: horizontal movement.
- Gamepad 1 right stick: look.
- Gamepad 1 left/right triggers: down/up movement.

## Size and resize state

- `setSize(width, height)` — programmatic resize.
- `framebufferExtent() const -> VkExtent2D` — current framebuffer size.
- `consumeFramebufferResized() -> bool` — returns and clears the resize flag set by the framebuffer callback.

Callers should treat zero extents as minimized/unrenderable and wait for a non-zero framebuffer before rebuilding swapchain resources.

## Window metadata

`setTitle(const char*)` updates the native title. The renderer uses a throttled title update with a fixed stack buffer for runtime stats.

## Native handle and Vulkan surface

- `handle() const -> GLFWwindow*` exposes the raw handle for backend glue.
- `createSurface(VkInstance, VkSurfaceKHR*) const` creates a Vulkan surface through GLFW.

Keep surface ownership in the renderer/backend that created the Vulkan instance; `Window` only asks GLFW to create the surface.
