#include "assets/SceneImporter.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace ve {
namespace {

std::string normalizedExtension(std::string extension) {
    if (extension.empty() || extension.front() != '.') {
        throw std::invalid_argument("Scene importer extension must begin with '.'");
    }
    std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

} // namespace

void SceneImporterRegistry::registerImporter(const SceneImporterDescriptor& descriptor) {
    if (descriptor.id.empty() || descriptor.version == 0U || descriptor.import == nullptr ||
        descriptor.extensions.empty()) {
        throw std::invalid_argument("Scene importer descriptor is incomplete");
    }
    if (std::ranges::any_of(importers_, [&](const SceneImporter& importer) {
            return importer.id == descriptor.id;
        })) {
        throw std::invalid_argument("Scene importer id is already registered: " +
                                    std::string{descriptor.id});
    }

    SceneImporter importer;
    importer.id = descriptor.id;
    importer.version = descriptor.version;
    importer.import = descriptor.import;
    importer.extensions.reserve(descriptor.extensions.size());
    for (const std::string_view extension : descriptor.extensions) {
        std::string normalized = normalizedExtension(std::string{extension});
        const bool duplicate = std::ranges::any_of(importers_, [&](const SceneImporter& current) {
            return std::ranges::find(current.extensions, normalized) != current.extensions.end();
        }) || std::ranges::find(importer.extensions, normalized) != importer.extensions.end();
        if (duplicate) {
            throw std::invalid_argument("Scene importer extension is already registered: " + normalized);
        }
        importer.extensions.push_back(std::move(normalized));
    }
    std::ranges::sort(importer.extensions);
    importers_.push_back(std::move(importer));
    std::ranges::sort(importers_, {}, &SceneImporter::id);
}

const SceneImporter& SceneImporterRegistry::importerFor(
    const std::filesystem::path& source) const {
    const std::string extension = normalizedExtension(source.extension().string());
    const auto found = std::ranges::find_if(importers_, [&](const SceneImporter& importer) {
        return std::ranges::binary_search(importer.extensions, extension);
    });
    if (found == importers_.end()) {
        throw std::runtime_error("No scene importer is registered for extension: " + extension);
    }
    return *found;
}

ImportedGltfScene SceneImporterRegistry::import(const std::filesystem::path& source,
                                                 const AssetId sceneId) const {
    return importerFor(source).import(source, sceneId);
}

} // namespace ve
