#include "core/Config.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int gFailureCount = 0;

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << "\n";
        ++gFailureCount;
    }
}

void expectFalse(const std::string_view context, const bool value) {
    if (value) {
        std::cerr << "[FAILED] " << context << "\n";
        ++gFailureCount;
    }
}

} // namespace

int main() {
    expectTrue("default exposure value is valid", ve::isValidExposure(1.0f));
    expectTrue("positive finite exposure is valid", ve::isValidExposure(0.125f));
    expectFalse("zero exposure is invalid", ve::isValidExposure(0.0f));
    expectFalse("negative exposure is invalid", ve::isValidExposure(-1.0f));
    expectFalse("infinite exposure is invalid", ve::isValidExposure(std::numeric_limits<float>::infinity()));
    expectFalse("NaN exposure is invalid", ve::isValidExposure(std::nanf("")));
    const std::filesystem::path executableRoot = ve::executableDirectory();
    expectTrue("executable directory is absolute", executableRoot.is_absolute());
    expectFalse("executable directory is not empty", executableRoot.empty());
    const ve::EngineConfig defaults{};
    expectTrue("default albedo texture is asset-relative", defaults.groundAlbedoTexture == "textures/ground_albedo.png");
    expectTrue("default normal texture is asset-relative", defaults.groundNormalTexture == "textures/ground_normal.png");
    expectTrue("default ORM texture is asset-relative", defaults.groundOrmTexture == "textures/ground_orm.png");
    expectTrue("default imported model path is asset-relative", defaults.importedModelPath == "models/imported_showcase.obj");
    const std::filesystem::path assetRoot = "/tmp/volkengine-assets";
    expectTrue("relative asset paths resolve under asset directory",
               ve::resolveAssetPath(assetRoot, defaults.groundAlbedoTexture) == assetRoot / "textures/ground_albedo.png");
    expectTrue("relative model path resolves under asset directory",
               ve::resolveAssetPath(assetRoot, defaults.importedModelPath) == assetRoot / "models/imported_showcase.obj");
    const std::filesystem::path absoluteOverride = "/opt/materials/albedo.png";
    expectTrue("absolute asset paths bypass asset directory",
               ve::resolveAssetPath(assetRoot, absoluteOverride) == absoluteOverride);
    expectTrue("empty asset path remains empty", ve::resolveAssetPath(assetRoot, {}).empty());

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Config CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
