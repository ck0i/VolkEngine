# VolkEngine

VolkEngine is a C++23 Vulkan renderer foundation for high-fidelity real-time rendering with explicit performance constraints. It currently provides the engine scaffold, GLFW platform layer, Vulkan backend, shader pipeline, CPU renderer contract tests, and runnable sandbox.

Detailed design notes are kept out of this onboarding README. Start at `docs/README.md` for the full documentation map:

- `docs/api/` — public API surface by header/subsystem
- `docs/topics/` — architecture, renderer pipeline, performance, shaders, and assets

## Requirements

- C++23 compiler
- CMake 3.28+
- Ninja
- Vulkan SDK/runtime with `glslc`
- GLFW and spdlog, either installed as packages or fetched by CMake

On Arch/CachyOS-like systems:

```sh
sudo pacman -S cmake ninja gcc vulkan-headers vulkan-icd-loader shaderc glfw spdlog
```

On Windows, install the Vulkan SDK, CMake, Ninja, and a C++23 compiler. CMake can fetch GLFW/spdlog if package discovery fails.

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

Common flags: `--frames N`, `--resize-smoke`, `--screenshot FILE.ppm`, `--hot-reload-shaders`, `--grid-rows N`, `--grid-columns N`, `--depth-prepass`, `--no-depth-prepass`, `--indirect-draws`, `--no-indirect-draws`, `--imgui`, `--no-imgui`, `--gpu-timestamps`, `--no-gpu-timestamps`, `--width N`, `--height N`, `--exposure F`, `--vsync`, `--no-vsync`, `--validation`, `--no-validation`, and `--help`.

Controls: `Esc` closes; `WASD` moves; `Q/E` move down/up; arrow keys look; hold right mouse button for captured mouse-look.

## Developer notes

- The tracked `.clangd` points clangd at `out/build/linux-debug/compile_commands.json`. For another preset or build directory, adjust local clangd config or keep a local root `compile_commands.json` symlink/copy; root `compile_commands.json` is ignored.
- Generated build trees, screenshots, caches, and `RUNLOG.md` are ignored.
- Source assets such as `assets/textures/ground_albedo.ppm` and GLSL shader sources under `engine/shaders/` are tracked.
