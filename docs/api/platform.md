# Platform API

Headers: `engine/platform/Window.hpp`, `engine/platform/Input.hpp`, and `engine/platform/InputActions.hpp`.

## Lifetime

`GlfwRuntime` owns process-wide GLFW initialization and termination. Only one runtime may exist per process. `Window` borrows it, owns one `GLFWwindow*`, and must be destroyed first. GLFW calls remain on the main thread.

`Window` tracks framebuffer size, resize state, input callbacks, cursor capture, and Vulkan surface creation. Copy and move are disabled.

## Events and input

- `pollEvents()` pumps GLFW without blocking.
- `waitEvents()` blocks and is used while the framebuffer is minimized.
- `pollInput()` returns one GLFW-free `InputState` snapshot.
- `shouldClose()` and `requestClose()` control termination.

`InputTracker` preserves keyboard and mouse press/release edges that occur between render frames. Cursor and scroll deltas accumulate until the snapshot is consumed. Focus loss releases held keyboard/mouse state and resets cursor tracking.

Four gamepad slots map to GLFW joystick slots 1–4 and are sampled once per render frame. Disconnect emits releases for held buttons; reconnect starts a new state lifetime. Radial stick and trigger dead zones rescale values to finite `[-1, 1]` output.

`InputActionMap` maps physical input to compact action IDs. It stores 64 actions and up to eight bindings per action inline. Setup validates IDs and bindings; `evaluate` is deterministic, `noexcept`, and allocation-free. Digital bindings contribute edges and scaled values. Axis bindings contribute continuous values after scale, inversion, and dead zone.

## Camera input

`updateCamera(camera, input, deltaSeconds)` applies keyboard and captured-mouse movement. Right mouse captures the cursor without an initial jump. Input value types do not depend on GLFW and can be constructed by tests or replay code.

## Window/backend bridge

- `framebufferExtent()` returns current pixel dimensions.
- `consumeFramebufferResized()` returns and clears the resize flag used by swapchain recreation.
- `setSize()` requests a native resize.
- `setTitle()` updates the window title.
- `handle()` exposes `GLFWwindow*` only for backend glue.
- `createSurface()` creates a Vulkan surface through GLFW.
