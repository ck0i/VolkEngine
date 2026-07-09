#include "core/Application.hpp"
#include "core/Log.hpp"
#include "renderer/SceneRenderer.hpp"

#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr std::size_t kMaxSandboxSceneItems = 4'194'304;

struct SandboxArgs {
    ve::EngineConfig config{};
    ve::RunOptions run{};
    bool help = false;
};

template <typename Integer>
Integer parseInteger(const std::string_view value, const std::string_view optionName) {
    Integer parsed{};
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        throw std::invalid_argument("Invalid value for " + std::string(optionName));
    }
    return parsed;
}

template <typename Integer>
Integer parsePositiveInteger(const std::string_view value, const std::string_view optionName) {
    const Integer parsed = parseInteger<Integer>(value, optionName);
    if (parsed == 0) {
        throw std::invalid_argument("Invalid positive value for " + std::string(optionName));
    }
    return parsed;
}

std::uint32_t parseWindowDimension(const std::string_view value, const std::string_view optionName) {
    const std::uint32_t parsed = parsePositiveInteger<std::uint32_t>(value, optionName);
    constexpr std::uint32_t kMaxGlfwWindowExtent = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (parsed > kMaxGlfwWindowExtent) {
        throw std::invalid_argument(std::string(optionName) + " exceeds GLFW's int extent range");
    }
    return parsed;
}

float parseFloat(const std::string_view value, const std::string_view optionName) {
    float parsed = 0.0f;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size() || !std::isfinite(parsed) || parsed <= 0.0f) {
        throw std::invalid_argument("Invalid positive value for " + std::string(optionName));
    }
    return parsed;
}

std::string_view requireValue(int& index, const int argc, char** argv, const std::string_view optionName) {
    if (index + 1 >= argc) {
        throw std::invalid_argument("Missing value for " + std::string(optionName));
    }
    return std::string_view{argv[++index]};
}

void validateConfig(const ve::EngineConfig& config) {
    const std::size_t requiredItems = ve::DemoSceneRenderer::requiredItemCount(config.materialGridRows, config.materialGridColumns);
    if (requiredItems > kMaxSandboxSceneItems) {
        throw std::runtime_error("Sandbox material grid would generate " + std::to_string(requiredItems)
                                 + " scene items; cap is " + std::to_string(kMaxSandboxSceneItems)
                                 + " to avoid exhausting host memory. Use smaller --grid-rows/--grid-columns.");
    }
    if (config.materialGridTileRows == 0 || config.materialGridTileColumns == 0) {
        throw std::runtime_error("Sandbox material grid tile dimensions must be positive.");
    }
}

SandboxArgs parseArguments(int argc, char** argv) {
    SandboxArgs args{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--resize-smoke") {
            args.run.resizeSmoke = true;
        } else if (arg == "--frames") {
            args.run.maxFrames = parseInteger<std::uint64_t>(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--width") {
            args.config.initialWidth = parseWindowDimension(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--height") {
            args.config.initialHeight = parseWindowDimension(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--grid-rows") {
            args.config.materialGridRows = parsePositiveInteger<std::uint32_t>(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--grid-columns") {
            args.config.materialGridColumns = parsePositiveInteger<std::uint32_t>(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--grid-tile-rows") {
            args.config.materialGridTileRows = parsePositiveInteger<std::uint32_t>(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--grid-tile-columns") {
            args.config.materialGridTileColumns = parsePositiveInteger<std::uint32_t>(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--auto-depth-prepass") {
            args.config.depthPrepassMode = ve::DepthPrepassMode::Auto;
        } else if (arg == "--depth-prepass") {
            args.config.depthPrepassMode = ve::DepthPrepassMode::ForceOn;
        } else if (arg == "--no-depth-prepass") {
            args.config.depthPrepassMode = ve::DepthPrepassMode::ForceOff;
        } else if (arg == "--vsync") {
            args.config.vsync = true;
        } else if (arg == "--no-vsync") {
            args.config.vsync = false;
        } else if (arg == "--validation") {
            args.config.validation = true;
        } else if (arg == "--no-validation") {
            args.config.validation = false;
        } else if (arg == "--indirect-draws") {
            args.config.indirectSceneDraws = true;
        } else if (arg == "--no-indirect-draws") {
            args.config.indirectSceneDraws = false;
        } else if (arg == "--imgui" || arg == "--debug-overlay") {
            args.config.debugOverlay = true;
        } else if (arg == "--no-imgui" || arg == "--no-debug-overlay") {
            args.config.debugOverlay = false;
        } else if (arg == "--gpu-timestamps") {
            args.config.gpuTimestamps = true;
        } else if (arg == "--no-gpu-timestamps") {
            args.config.gpuTimestamps = false;
        } else if (arg == "--exposure") {
            args.config.exposure = parseFloat(requireValue(i, argc, argv, arg), arg);
        } else if (arg == "--screenshot") {
            args.run.screenshotPath = std::filesystem::path{requireValue(i, argc, argv, arg)};
        } else if (arg == "--hot-reload-shaders") {
            args.config.shaderHotReload = true;
        } else {
            throw std::invalid_argument("Unknown option " + std::string(arg));
        }
    }
    return args;
}

void printUsage() {
    std::cout << "Usage: VolkEngineSandbox [--frames N] [--resize-smoke] [--screenshot FILE.ppm] [--hot-reload-shaders] [--grid-rows N] [--grid-columns N] [--grid-tile-rows N] [--grid-tile-columns N] [--auto-depth-prepass|--depth-prepass|--no-depth-prepass] [--indirect-draws|--no-indirect-draws] [--imgui|--no-imgui] [--gpu-timestamps|--no-gpu-timestamps] [--width N] [--height N] [--exposure F] [--vsync|--no-vsync] [--validation|--no-validation]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        ve::initializeLogging();
        SandboxArgs args = parseArguments(argc, argv);
        if (args.help) {
            printUsage();
            return 0;
        }
        validateConfig(args.config);
        ve::Application app{args.config};
        return app.run(args.run);
    } catch (const std::exception& e) {
        ve::logger()->critical("Fatal error: {}", e.what());
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
