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
    DepthPrepassMode depthPrepassMode = DepthPrepassMode::Auto;
    std::filesystem::path shaderDirectory = executableDirectory() / "shaders";
    std::filesystem::path assetDirectory = executableDirectory() / "assets";
    std::filesystem::path groundAlbedoTexture = "textures/ground_albedo.png";
    std::filesystem::path groundNormalTexture = "textures/ground_normal.png";
    std::filesystem::path groundOrmTexture = "textures/ground_orm.png";
    std::filesystem::path importedModelPath = "models/imported_showcase.obj";
    std::filesystem::path cacheDirectory = executableDirectory() / "cache";
};

[[nodiscard]] inline bool isValidExposure(const float exposure) noexcept {
    return std::isfinite(exposure) && exposure > 0.0f;
}
namespace config_detail {

[[nodiscard]] inline bool pathWithin(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto rootIterator = root.begin();
    auto candidateIterator = candidate.begin();
    for (; rootIterator != root.end(); ++rootIterator, ++candidateIterator) {
        if (candidateIterator == candidate.end() || *rootIterator != *candidateIterator) {
            return false;
        }
    }
    return true;
}

} // namespace config_detail


[[nodiscard]] inline std::filesystem::path resolveAssetPath(const std::filesystem::path& assetDirectory,
                                                             const std::filesystem::path& configuredPath) {
    if (configuredPath.empty() || configuredPath.is_absolute()) {
        return configuredPath;
    }
    if (assetDirectory.empty()) {
        return {};
    }

    std::error_code error;
    const std::filesystem::path absoluteRoot = std::filesystem::absolute(assetDirectory, error);
    if (error) {
        return {};
    }
    const std::filesystem::path root = absoluteRoot.lexically_normal();
    const std::filesystem::path candidate = (root / configuredPath).lexically_normal();
    if (!config_detail::pathWithin(root, candidate)) {
        return {};
    }
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return {};
    }
    error.clear();
    const std::filesystem::path canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
    if (error || !config_detail::pathWithin(canonicalRoot, canonicalCandidate)) {
        return {};
    }
    return candidate;
}

struct RunOptions {
    std::uint64_t maxFrames = 0;
    bool resizeSmoke = false;
    bool acquireRecoverySmoke = false;
    std::filesystem::path screenshotPath;
};

} // namespace ve
