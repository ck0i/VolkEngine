#include "assets/ReferenceAssetPipeline.hpp"

#include "assets/DerivedDataCache.hpp"
#include "assets/SceneImporter.hpp"
#include "assets/TextureArtifact.hpp"
#include "assets/RuntimeAssets.hpp"
#include "core/FileSystem.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace ve {
namespace {

constexpr std::string_view kSettings = "normals=generate;tangents=generate;coordinates=gltf-rhs-y-up";

const SceneImporter& referenceImporter() {
    static const SceneImporterRegistry registry = [] {
        SceneImporterRegistry value;
        registerGltfImporter(value);
        return value;
    }();
    return registry.importerFor("reference.gltf");
}


std::vector<std::byte> readSource(const std::filesystem::path& path) {
    return readBinaryFile(path, 256U * 1024U * 1024U);
}

ContentHash sourceHash(const std::filesystem::path& path) { return hashBytes(readSource(path)); }

ContentHash keyFor(const AssetRecord& record, const ContentHash payloadHash,
                   const ArtifactType type, const std::vector<ContentHash>& dependencies,
                   const std::string& target) {
    return makeDerivedDataKey({payloadHash, record.importerId, record.importerVersion,
                               record.settingsHash, dependencies, type,
                               record.artifactSchemaVersion, target,
                               type == ArtifactType::Mesh ? "vertex48-index32" : "portable"});
}

ImportedGltfScene loadBundle(const AssetDatabase& database, const DerivedDataCache& cache) {
    const AssetRecord* sceneRecord = database.find(builtin_assets::kReferenceSceneId);
    if (sceneRecord == nullptr) throw std::runtime_error("Reference scene record is missing");
    ImportedGltfScene scene = deserializeSceneArtifact(
        cache.load(sceneRecord->artifactKey, ArtifactType::Scene,
                   sceneRecord->artifactSchemaVersion).payload);
    for (const AssetRecord& record : database.records()) {
        if (record.type == AssetType::Mesh) {
            scene.meshes.push_back(deserializeMeshArtifact(
                cache.load(record.artifactKey, ArtifactType::Mesh,
                           record.artifactSchemaVersion).payload));
        } else if (record.type == AssetType::Material) {
            scene.materials.push_back(deserializeMaterialArtifact(
                cache.load(record.artifactKey, ArtifactType::Material,
                           record.artifactSchemaVersion).payload));
        } else if (record.type == AssetType::Texture) {
            const TextureArtifact texture = deserializeTextureArtifact(
                cache.load(record.artifactKey, ArtifactType::Texture,
                           record.artifactSchemaVersion).payload);
            if (texture.id != record.id) {
                throw std::runtime_error(
                    "Texture artifact identity does not match its asset record");
            }
        }
    }
    std::ranges::sort(scene.meshes, {}, &ImportedMeshPrimitive::id);
    std::ranges::sort(scene.materials, {}, &ImportedMaterial::id);
    return scene;
}

bool databaseIsCurrent(const AssetDatabase& database, const std::filesystem::path& assetRoot,
                       const ContentHash sceneSourceHash, const ContentHash settingsHash,
                       const SceneImporter& importer, const std::string& target) {
    const AssetRecord* scene = database.find(builtin_assets::kReferenceSceneId);
    if (scene == nullptr || scene->sourceHash != sceneSourceHash || scene->settingsHash != settingsHash ||
        scene->importerId != importer.id || scene->importerVersion != importer.version ||
        scene->target != target || scene->state != AssetState::Ready) {
        return false;
    }
    for (const AssetRecord& record : database.records()) {
        if (record.state != AssetState::Ready || record.target != target ||
            record.importerId != importer.id || record.importerVersion != importer.version) {
            return false;
        }
        if (!record.sourcePath.empty()) {
            std::error_code error;
            const std::filesystem::path path = assetRoot / record.sourcePath;
            if (!std::filesystem::is_regular_file(path, error) || error || sourceHash(path) != record.sourceHash) return false;
        }
    }
    return true;
}

AssetRecord baseRecord(const AssetId id, const AssetType type, std::filesystem::path sourcePath,
                       const ContentHash source, const ContentHash settingsHash,
                       const SceneImporter& importer, const std::string& target) {
    AssetRecord record;
    record.id = id;
    record.type = type;
    switch (type) {
    case AssetType::Texture:
        record.artifactSchemaVersion = TextureArtifact::kSchemaVersion;
        break;
    case AssetType::Mesh:
        record.artifactSchemaVersion = ImportedGltfScene::kMeshArtifactSchemaVersion;
        break;
    case AssetType::Material:
        record.artifactSchemaVersion = ImportedGltfScene::kMaterialArtifactSchemaVersion;
        break;
    case AssetType::Scene:
        record.artifactSchemaVersion = ImportedGltfScene::kSceneArtifactSchemaVersion;
        break;
    default: throw std::invalid_argument("Unsupported reference asset type");
    }
    record.sourcePath = std::move(sourcePath);
    record.sourceHash = source;
    record.importerId = importer.id;
    record.importerVersion = importer.version;
    record.normalizedSettings = std::string{kSettings};
    record.settingsHash = settingsHash;
    record.target = target;
    record.state = AssetState::Ready;
    return record;
}

} // namespace

MeshAssetHandle referenceMeshHandle(const ImportedGltfScene& scene, const AssetId meshId) {
    const auto found = std::ranges::find(scene.meshes, meshId, &ImportedMeshPrimitive::id);
    if (found == scene.meshes.end()) {
        throw std::runtime_error("Mesh asset is not present in the reference scene");
    }
    const std::size_t ordinal = static_cast<std::size_t>(found - scene.meshes.begin());
    constexpr std::uint32_t kFirstAuthoredMeshIndex = builtin_assets::kReferenceMesh.index;
    if (ordinal > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() -
                                           kFirstAuthoredMeshIndex)) {
        throw std::runtime_error("Reference scene mesh handle range is exhausted");
    }
    return {kFirstAuthoredMeshIndex + static_cast<std::uint32_t>(ordinal), 1U};
}

ReferenceAssetBundle cookReferenceAssets(const std::filesystem::path& assetRoot,
                                          const std::filesystem::path& cacheRoot,
                                          std::string targetPlatform) {
    const auto start = std::chrono::steady_clock::now();
    if (targetPlatform.empty()) throw std::invalid_argument("Asset target platform must not be empty");
    const std::filesystem::path scenePath = assetRoot / "reference_scene.gltf";
    const SceneImporter& importer = referenceImporter();
    const ContentHash sceneHash = sourceHash(scenePath);
    const ContentHash settingsHash = hashString(kSettings);
    DerivedDataCache cache{cacheRoot / "ddc"};
    const std::filesystem::path databasePath = cacheRoot / "asset_database.veasdb";

    ReferenceAssetBundle result;
    std::error_code existsError;
    const auto accountPublication = [&](const bool published) {
        if (published) {
            ++result.metrics.cacheMisses;
            ++result.metrics.rebuiltAssets;
        } else {
            ++result.metrics.cacheHits;
        }
    };
    if (std::filesystem::is_regular_file(databasePath, existsError) && !existsError) {
        try {
            AssetDatabase existing = AssetDatabase::load(databasePath);
            if (databaseIsCurrent(existing, assetRoot, sceneHash, settingsHash, importer,
                                  targetPlatform)) {
                result.scene = loadBundle(existing, cache);
                result.database = std::move(existing);
                result.metrics.cacheHits = static_cast<std::uint32_t>(result.database.records().size());
                result.metrics.cookMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                return result;
            }
        } catch (const std::runtime_error&) {
            // The source database is authoritative; cache metadata and artifacts
            // are disposable and are rebuilt transactionally below.
        }
    }

    ImportedGltfScene imported = importer.import(scenePath, builtin_assets::kReferenceSceneId);
    std::ranges::sort(imported.meshes, {}, &ImportedMeshPrimitive::id);
    std::ranges::sort(imported.materials, {}, &ImportedMaterial::id);
    std::vector<AssetRecord> records;
    records.reserve(imported.meshes.size() + imported.materials.size() * 2U + 1U);

    for (const ImportedMaterial& material : imported.materials) {
        for (const ImportedTextureReference& texture : material.textures) {
            const std::filesystem::path textureSourcePath = texture.sourcePath;
            const ContentHash textureHash = sourceHash(assetRoot / textureSourcePath);
            AssetRecord textureRecord = baseRecord(texture.id, AssetType::Texture,
                                                   textureSourcePath, textureHash, settingsHash,
                                                   importer, targetPlatform);
            const TextureArtifact importedTexture = importTextureArtifact(
                assetRoot / textureSourcePath, texture.id, texture.role, texture.colorSpace);
            const std::vector<std::byte> texturePayload =
                serializeTextureArtifact(importedTexture);
            textureRecord.artifactKey = keyFor(textureRecord, hashBytes(texturePayload),
                                               ArtifactType::Texture, {}, targetPlatform);
            accountPublication(cache.publish(textureRecord.artifactKey, ArtifactType::Texture,
                                             textureRecord.artifactSchemaVersion, texturePayload));
            textureRecord.artifactPath = cache.artifactPath(textureRecord.artifactKey, ArtifactType::Texture)
                                             .lexically_relative(cacheRoot);
            records.push_back(std::move(textureRecord));
        }
    }
    std::ranges::sort(records, {}, &AssetRecord::id);
    records.erase(std::unique(records.begin(), records.end(),
                              [](const AssetRecord& left, const AssetRecord& right) {
                                  return left.id == right.id;
                              }), records.end());

    for (const ImportedMaterial& material : imported.materials) {
        AssetRecord materialRecord = baseRecord(material.id, AssetType::Material,
                                                "reference_scene.gltf", sceneHash, settingsHash,
                                                importer, targetPlatform);
        std::vector<ContentHash> dependencyHashes;
        for (const ImportedTextureReference& texture : material.textures) {
            materialRecord.dependencies.push_back(texture.id);
            const auto found = std::ranges::find(records, texture.id, &AssetRecord::id);
            if (found == records.end()) throw std::runtime_error("Imported material texture record is missing");
            dependencyHashes.push_back(found->artifactKey);
        }
        const std::vector<std::byte> payload = serializeMaterialArtifact(material);
        materialRecord.artifactKey = keyFor(materialRecord, hashBytes(payload),
                                            ArtifactType::Material, dependencyHashes,
                                            targetPlatform);
        accountPublication(cache.publish(materialRecord.artifactKey, ArtifactType::Material,
                                         materialRecord.artifactSchemaVersion, payload));
        materialRecord.artifactPath = cache.artifactPath(materialRecord.artifactKey, ArtifactType::Material)
                                          .lexically_relative(cacheRoot);
        records.push_back(std::move(materialRecord));
    }

    for (const ImportedMeshPrimitive& mesh : imported.meshes) {
        AssetRecord meshRecord = baseRecord(mesh.id, AssetType::Mesh, "reference_scene.gltf",
                                            sceneHash, settingsHash, importer, targetPlatform);
        std::vector<ContentHash> dependencyHashes;
        if (mesh.material.valid()) {
            meshRecord.dependencies.push_back(mesh.material);
            const auto found = std::ranges::find(records, mesh.material, &AssetRecord::id);
            if (found == records.end()) throw std::runtime_error("Imported mesh material record is missing");
            dependencyHashes.push_back(found->artifactKey);
        }
        const std::vector<std::byte> payload = serializeMeshArtifact(mesh);
        meshRecord.artifactKey = keyFor(meshRecord, hashBytes(payload), ArtifactType::Mesh,
                                        dependencyHashes, targetPlatform);
        accountPublication(cache.publish(meshRecord.artifactKey, ArtifactType::Mesh,
                                         meshRecord.artifactSchemaVersion, payload));
        meshRecord.artifactPath = cache.artifactPath(meshRecord.artifactKey, ArtifactType::Mesh)
                                      .lexically_relative(cacheRoot);
        records.push_back(std::move(meshRecord));
    }

    AssetRecord sceneRecord = baseRecord(imported.sceneId, AssetType::Scene,
                                         "reference_scene.gltf", sceneHash, settingsHash,
                                         importer, targetPlatform);
    std::vector<ContentHash> sceneDependencies;
    for (const AssetRecord& record : records) {
        if (record.type == AssetType::Mesh || record.type == AssetType::Material) {
            sceneRecord.dependencies.push_back(record.id);
            sceneDependencies.push_back(record.artifactKey);
        }
    }
    std::ranges::sort(sceneRecord.dependencies);
    const std::vector<std::byte> scenePayload = serializeSceneArtifact(imported);
    sceneRecord.artifactKey = keyFor(sceneRecord, hashBytes(scenePayload), ArtifactType::Scene,
                                     sceneDependencies, targetPlatform);
    accountPublication(cache.publish(sceneRecord.artifactKey, ArtifactType::Scene,
                                     sceneRecord.artifactSchemaVersion, scenePayload));
    sceneRecord.artifactPath = cache.artifactPath(sceneRecord.artifactKey, ArtifactType::Scene)
                                   .lexically_relative(cacheRoot);
    records.push_back(std::move(sceneRecord));

    AssetDatabase replacement;
    replacement.replaceAll(std::move(records));
    std::filesystem::create_directories(cacheRoot);
    replacement.saveAtomic(databasePath);
    result.database = std::move(replacement);
    result.scene = imported;
    result.metrics.cookMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}
ReferenceAssetReloader::ReferenceAssetReloader(std::filesystem::path assetRoot,
                                               std::filesystem::path cacheRoot,
                                               std::string targetPlatform)
    : assetRoot_(std::move(assetRoot)), cacheRoot_(std::move(cacheRoot)),
      targetPlatform_(std::move(targetPlatform)),
      active_(cookReferenceAssets(assetRoot_, cacheRoot_, targetPlatform_)) {}

AssetReloadResult ReferenceAssetReloader::reload() noexcept {
    AssetReloadResult result;
    try {
        const std::vector<std::byte> previousManifest = active_.database.serialize();
        ReferenceAssetBundle candidate =
            cookReferenceAssets(assetRoot_, cacheRoot_, targetPlatform_);
        result.metrics = candidate.metrics;
        if (candidate.database.serialize() == previousManifest) {
            result.status = AssetReloadStatus::Unchanged;
            return result;
        }
        active_ = std::move(candidate);
        ++generation_;
        result.status = AssetReloadStatus::Published;
        return result;
    } catch (const std::exception& error) {
        result.status = AssetReloadStatus::Failed;
        result.diagnostic = "Asset reload failed; source=" +
            (assetRoot_ / "reference_scene.gltf").generic_string() +
            "; importer=" + referenceImporter().id + "@" +
            std::to_string(referenceImporter().version) + "; cache=" + cacheRoot_.generic_string() +
            "; dependency_chain=scene->material->texture; reason=" + error.what();
        return result;
    } catch (...) {
        result.status = AssetReloadStatus::Failed;
        result.diagnostic = "Asset reload failed with a non-standard exception; source=" +
            (assetRoot_ / "reference_scene.gltf").generic_string() +
            "; importer=" + referenceImporter().id + "@" +
            std::to_string(referenceImporter().version) + "; cache=" +
            cacheRoot_.generic_string();
        return result;
    }
}


} // namespace ve
