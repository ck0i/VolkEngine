#pragma once
#include "core/FileSystem.hpp"


#include <cstdint>
#include <filesystem>
#include <string>

namespace ve {

enum class DepthPrepassMode : std::uint8_t {
    ForceOff,
    ForceOn
};

struct EngineConfig {
    std::string applicationName = "VolkEngine Sandbox";
    std::uint32_t initialWidth = 1280;
    std::uint32_t initialHeight = 720;
    bool validation = VOLKENGINE_VALIDATION != 0;
    bool vsync = true;
    float exposure = 1.0f;
    bool shaderHotReload = false;
    bool indirectSceneDraws = true;
    bool debugOverlay = true;
    bool gpuTimestamps = true;
    std::uint32_t materialGridRows = 4;
    std::uint32_t materialGridColumns = 5;
    std::uint32_t materialGridTileRows = 16;
    std::uint32_t materialGridTileColumns = 16;
    DepthPrepassMode depthPrepassMode = DepthPrepassMode::ForceOff;
    std::filesystem::path shaderDirectory = executableDirectory() / "shaders";
    std::filesystem::path assetDirectory = executableDirectory() / "assets";
    std::filesystem::path cacheDirectory = executableDirectory() / "cache";
};

struct RunOptions {
    std::uint64_t maxFrames = 0;
    bool resizeSmoke = false;
    std::filesystem::path screenshotPath;
};

} // namespace ve
