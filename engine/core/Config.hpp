#pragma once
#include "core/FileSystem.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace ve {

#if defined(VOLKENGINE_VALIDATION)
inline constexpr bool kDefaultValidationEnabled = VOLKENGINE_VALIDATION != 0;
#else
inline constexpr bool kDefaultValidationEnabled = false;
#endif

enum class DepthPrepassMode : std::uint8_t {
    Auto,
    ForceOff,
    ForceOn
};

struct EngineConfig {
    std::string applicationName = "VolkEngine Sandbox";
    std::uint32_t initialWidth = 1280;
    std::uint32_t initialHeight = 720;
    bool validation = kDefaultValidationEnabled;
    bool requireValidation = false;
    bool vsync = true;
    float exposure = 1.0f;
    bool shaderHotReload = false;
    bool indirectSceneDraws = true;
    bool shadows = true;
    bool gpuVisibilityValidation = false;
    bool depthPyramidOcclusion = true;
    bool gpuClusterCommands = false;
    bool debugOverlay = true;
    bool gpuTimestamps = true;
    double fixedSimulationStepSeconds = 1.0 / 60.0;
    double maximumSimulationAccumulatedSeconds = 0.25;
    std::uint32_t maximumSimulationSubsteps = 8;
    std::uint32_t materialGridRows = 4;
    std::uint32_t materialGridColumns = 5;
    std::uint32_t materialGridTileRows = 16;
    std::uint32_t materialGridTileColumns = 16;
    DepthPrepassMode depthPrepassMode = DepthPrepassMode::Auto;
    std::filesystem::path shaderDirectory = executableDirectory() / "shaders";
    std::filesystem::path assetDirectory = executableDirectory() / "assets";
    std::filesystem::path cacheDirectory = executableDirectory() / "cache";
};

[[nodiscard]] inline bool isValidExposure(const float exposure) noexcept {
    return std::isfinite(exposure) && exposure > 0.0f;
}

struct RunOptions {
    std::uint64_t maxFrames = 0;
    std::uint64_t warmupFrames = 0;
    std::string scenarioName = "interactive";
    std::uint64_t screenshotFrame = 0;
    bool resizeSmoke = false;
    bool acquireRecoverySmoke = false;
    std::filesystem::path summaryPath;
    std::filesystem::path screenshotPath;
};

} // namespace ve
