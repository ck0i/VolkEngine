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

GLFW callbacks update `InputTracker` as events arrive, preserving presses and releases that both occur between frames. `pollInput()` consumes one `InputState` value snapshot: held state persists, while pressed/released transitions plus accumulated cursor and two-axis scroll deltas clear after consumption. The snapshot safely exposes supported keyboard and mouse-button state, cursor position/delta, scroll motion, and capture status without GLFW types. Non-finite events are ignored, and finite accumulation overflow resets the affected motion instead of poisoning a frame.

`updateCamera(Camera& camera, const InputState& input, float dt)` applies the same snapshot that gameplay may receive through `Application::runWithInput(...)`. The compatibility overload `updateCamera(Camera& camera, float dt)` consumes the current tracker snapshot itself. `mapCameraInput(...)` converts actions into normalized signed axes, while `applyCameraInput(...)` applies axes and mouse deltas transactionally. Non-finite axes or delta time, negative delta time, and camera-step overflow are rejected before the caller's camera changes. Focus loss releases held inputs, clears cursor and scroll deltas, and exits captured/raw mouse mode.

Current sandbox controls:

- `Esc`: close.
- `WASD`: horizontal movement.
- `Q` / `E`: down/up movement.
- Arrow keys: look.
- Hold right mouse button: captured mouse-look.
- `Space` with `--world-scene`: pause/resume cube rotation.

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
