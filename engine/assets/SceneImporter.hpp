#pragma once

#include "assets/GltfImporter.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ve {

using SceneImportFunction = ImportedGltfScene (*)(const std::filesystem::path&, AssetId);

struct SceneImporterDescriptor {
    std::string_view id;
    std::uint32_t version = 0;
    std::span<const std::string_view> extensions;
    SceneImportFunction import = nullptr;
};

struct SceneImporter {
    std::string id;
    std::uint32_t version = 0;
    std::vector<std::string> extensions;
    SceneImportFunction import = nullptr;
};

class SceneImporterRegistry {
public:
    void registerImporter(const SceneImporterDescriptor& descriptor);

    [[nodiscard]] const SceneImporter& importerFor(const std::filesystem::path& source) const;
    [[nodiscard]] ImportedGltfScene import(const std::filesystem::path& source,
                                           AssetId sceneId) const;
    [[nodiscard]] std::span<const SceneImporter> importers() const noexcept { return importers_; }

private:
    std::vector<SceneImporter> importers_;
};

} // namespace ve
