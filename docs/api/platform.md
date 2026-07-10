# Platform API

Headers: `engine/platform/Window.hpp`, `engine/platform/Input.hpp`.

`GlfwRuntime` owns GLFW's process-global initialization. `Window` owns one `GLFWwindow*`, collects native input, tracks framebuffer resize state, and creates Vulkan surfaces for the backend. The GLFW-free `CameraInput` mapper is the deterministic engine-side input policy.

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

`updateCamera(Camera& camera, float dt)` collects current keyboard/mouse state and applies it to the supplied camera.

The reusable `mapCameraInput(...)` helper converts boolean actions into normalized signed axes, while `applyCameraInput(...)` applies those axes and mouse deltas to a camera. This split keeps GLFW polling platform-owned and makes camera input deterministic to test and replay.

Current sandbox controls:

- `Esc`: close.
- `WASD`: horizontal movement.
- `Q` / `E`: down/up movement.
- Arrow keys: look.
- Hold right mouse button: captured mouse-look.

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
