# VolkEngine

VolkEngine is a C++23 game-engine project built around a Vulkan 1.3 renderer. The repository contains the engine library, a sandbox, an interim editor, and a streamed-landscape benchmark.

See [ROADMAP.md](ROADMAP.md) for current capabilities and planned work. Technical notes are indexed in [docs/README.md](docs/README.md).

## Requirements

- CMake 3.28+
- A C++23 compiler
- A Vulkan-capable GPU and driver for renderer executables
- X11 or Wayland development files when building fetched GLFW on Linux

CMake fetches pinned third-party dependencies when system packages are unavailable. Tools are stored under `out/bootstrap/`; build dependencies and outputs are stored under `out/build/<preset>/`. Configure reports the required package command when Linux window-system headers are missing. It never installs system packages.

## Build

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Release builds use `linux-release`. Windows provides matching `windows-debug` and `windows-release` presets.

List all presets with:

```sh
cmake --list-presets
cmake --list-presets=build
cmake --list-presets=test
```

## Test

```sh
ctest --preset linux-debug
```

Use the preset matching the configured build directory.

## Run

```sh
./out/build/linux-debug/VolkEngineSandbox
```

Windows:

```powershell
.\out\build\windows-debug\VolkEngineSandbox.exe
```

Useful smoke runs:

```sh
# Fixed-duration renderer run with swapchain resize coverage
./out/build/linux-debug/VolkEngineSandbox --frames 120 --resize-smoke

# World simulation and extraction
./out/build/linux-debug/VolkEngineSandbox --world-scene --frames 120 --no-imgui

# Save and reload the persisted world subset
./out/build/linux-debug/VolkEngineSandbox --world-scene --frames 120 \
  --save-scene /tmp/example.vescene --no-imgui
./out/build/linux-debug/VolkEngineSandbox --load-scene /tmp/example.vescene \
  --frames 120 --no-imgui
```

Run `VolkEngineSandbox --help` for the complete option list. Help exits before GLFW or Vulkan initialization.

Controls: `WASD` moves, `Q`/`E` moves vertically, arrow keys or captured right-mouse input look, and `Esc` exits. In `--world-scene` mode, `Space` pauses simulation.

## Repository layout

- `engine/` — runtime, renderer, assets, scene, landscape, editor, and platform code
- `samples/` — sandbox, editor, and partition benchmark executables
- `tests/` — CPU contract tests and benchmarks
- `assets/` — source models and textures
- `docs/` — architecture and subsystem notes
- `schemas/` — external authoring schemas
- `tools/` — test and asset utilities

CMake emits `compile_commands.json` under the selected build directory. The tracked `.clangd` points at `out/build/linux-debug`; adjust local clangd configuration when using another preset.
