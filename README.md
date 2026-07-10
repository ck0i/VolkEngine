# VolkEngine

VolkEngine is a C++23 Vulkan renderer foundation for high-fidelity real-time rendering with explicit performance constraints. It currently provides the engine scaffold, GLFW platform layer, Vulkan backend, shader pipeline, CPU renderer contract tests, and runnable sandbox.

Detailed design notes are kept out of this onboarding README. Start at `docs/README.md` for the full documentation map:

- `docs/api/` — public API surface by header/subsystem
- `docs/topics/` — architecture, renderer pipeline, performance, shaders, and assets

## Requirements

- CMake 3.28+
- A C++23 compiler for the host platform
- A Vulkan-capable GPU driver/ICD to run the sandbox

The CMake presets bootstrap the remaining build dependencies into ignored repo-local directories:

- `out/bootstrap/` — Ninja and the LunarG Vulkan SDK when the host does not already provide them
- `out/build/<preset>/_deps/` — GLFW, spdlog, Vulkan Memory Allocator, and Dear ImGui from pinned release archives when package discovery fails

Linux native-window note: fetched GLFW still needs one system window backend at compile time. If neither X11 nor Wayland development files are installed, configure stops early with a distro-specific command for Debian/Ubuntu, Fedora/RHEL, Arch/CachyOS/Manjaro, openSUSE, or Alpine. CMake does not run `sudo` or mutate the OS package database from configure.

Windows note: install a C++23-capable toolchain and GPU driver. The `windows-*` presets can provision Ninja plus a repo-local Vulkan SDK if they are not already installed.

## Build

List available presets:

```sh
cmake --list-presets
cmake --list-presets=build
cmake --list-presets=test
```

Linux debug:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Linux release:

```sh
cmake --preset linux-release
cmake --build --preset linux-release
```

Windows hosts expose matching `windows-debug` and `windows-release` Ninja presets. Build output is written under `out/build/<preset>`.

## Test

```sh
ctest --preset linux-debug
```

The `linux-debug` and `linux-release` test presets run the registered CPU tests with `--output-on-failure`.

## Run

Linux:

```sh
./out/build/linux-debug/VolkEngineSandbox
```

Windows:

```powershell
.\out\build\windows-debug\VolkEngineSandbox.exe
```

Automated Linux smoke run:

```sh
./out/build/linux-debug/VolkEngineSandbox --frames 120 --resize-smoke
```

Acquire/semaphore recovery smoke run:

```sh
./out/build/linux-debug/VolkEngineSandbox --frames 3 --acquire-recovery-smoke --screenshot /tmp/volk-recovery.ppm --no-imgui
```

World-backed extraction smoke run:

```sh
./out/build/linux-debug/VolkEngineSandbox --world-scene --frames 120 --no-imgui
```
This path compiles and runs the sandbox's named `spin` world system through `WorldSystemScheduler`, then performs interpolated world extraction and normal Vulkan submission.

Common flags: `--frames N`, `--world-scene`, `--resize-smoke`, `--acquire-recovery-smoke`, `--screenshot FILE.ppm`, `--hot-reload-shaders`, `--grid-rows N`, `--grid-columns N`, `--auto-depth-prepass`, `--depth-prepass`, `--no-depth-prepass`, `--indirect-draws`, `--no-indirect-draws`, `--imgui`, `--no-imgui`, `--gpu-timestamps`, `--no-gpu-timestamps`, `--width N`, `--height N`, `--exposure F`, `--vsync`, `--no-vsync`, `--validation`, `--no-validation`, and `--help`.
`--help` / `-h` is terminal: it prints usage and exits successfully without initializing GLFW or Vulkan, even when copied trailing arguments are malformed or unknown.

Controls: `Esc` closes; `WASD` moves; `Q/E` move down/up; arrow keys look; hold right mouse button for captured mouse-look; with `--world-scene`, `Space` pauses/resumes the cube rotation.

## Developer notes

- clangd support becomes useful after configuring a CMake preset, because CMake writes `compile_commands.json` under `out/build/<preset>/`. The tracked `.clangd` defaults to `out/build/linux-debug`; if you use another preset, adjust your local clangd config or keep an ignored root `compile_commands.json` symlink/copy.
- Generated build trees, screenshots, and caches are ignored.
- Source assets such as `assets/textures/ground_albedo.png`, `assets/textures/ground_normal.png`, OBJ meshes under `assets/models/`, and GLSL shader sources under `engine/shaders/` are tracked.
