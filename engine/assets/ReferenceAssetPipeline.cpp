#include "assets/ReferenceAssetPipeline.hpp"

#include "assets/DerivedDataCache.hpp"
#include "assets/RuntimeAssets.hpp"
#include "assets/SceneImporter.hpp"
#include "assets/TextureArtifact.hpp"
#include "core/FileSystem.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <type_traits>

namespace ve {
namespace {

constexpr std::string_view kSettings =
    "normals=generate;tangents=generate;coordinates=gltf-rhs-y-up";

const SceneImporter &referenceImporter() {
  static const SceneImporterRegistry registry = [] {
        SceneImporterRegistry value;
        registerGltfImporter(value);
        return value;
    }();
  return registry.importerFor("reference.gltf");
}

std::vector<std::byte> readSource(const std::filesystem::path &path) {
  return readBinaryFile(path, 256U * 1024U * 1024U);
}

ContentHash sourceHash(const std::filesystem::path &path) {
  return hashBytes(readSource(path));
}

ContentHash keyFor(const AssetRecord &record, const ContentHash payloadHash,
                   const ArtifactType type,
                   const std::vector<ContentHash> &dependencies,
                   const std::string &target) {
  return makeDerivedDataKey(
      {payloadHash, record.importerId, record.importerVersion,
       record.settingsHash, dependencies, type, record.artifactSchemaVersion,
       target, type == ArtifactType::Mesh ? "vertex48-index32" : "portable"});
}

ImportedGltfScene loadBundle(const AssetDatabase &database,
                             const DerivedDataCache &cache) {
  const AssetRecord *sceneRecord =
      database.find(builtin_assets::kReferenceSceneId);
  if (sceneRecord == nullptr)
    throw std::runtime_error("Reference scene record is missing");
  ImportedGltfScene scene = deserializeSceneArtifact(
      cache
          .load(sceneRecord->artifactKey, ArtifactType::Scene,
                sceneRecord->artifactSchemaVersion)
          .payload);
  for (const AssetRecord &record : database.records()) {
    if (record.type == AssetType::Mesh) {
      scene.meshes.push_back(deserializeMeshArtifact(
          cache
              .load(record.artifactKey, ArtifactType::Mesh,
                    record.artifactSchemaVersion)
              .payload));
    } else if (record.type == AssetType::Material) {
      scene.materials.push_back(deserializeMaterialArtifact(
          cache
              .load(record.artifactKey, ArtifactType::Material,
                    record.artifactSchemaVersion)
              .payload));
    } else if (record.type == AssetType::Texture) {
      const TextureArtifact texture = deserializeTextureArtifact(
          cache
              .load(record.artifactKey, ArtifactType::Texture,
                    record.artifactSchemaVersion)
              .payload);
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

bool databaseIsCurrent(const AssetDatabase &database,
                       const std::filesystem::path &assetRoot,
                       const ContentHash sceneSourceHash,
                       const ContentHash settingsHash,
                       const SceneImporter &importer,
                       const std::string &target) {
  const AssetRecord *scene = database.find(builtin_assets::kReferenceSceneId);
  if (scene == nullptr || scene->sourceHash != sceneSourceHash ||
      scene->settingsHash != settingsHash || scene->importerId != importer.id ||
      scene->importerVersion != importer.version || scene->target != target ||
      scene->state != AssetState::Ready) {
    return false;
  }
  for (const AssetRecord &record : database.records()) {
    if (record.state != AssetState::Ready || record.target != target ||
        record.importerId != importer.id ||
        record.importerVersion != importer.version) {
      return false;
    }
    if (!record.sourcePath.empty()) {
      std::error_code error;
      const std::filesystem::path path = assetRoot / record.sourcePath;
      if (!std::filesystem::is_regular_file(path, error) || error ||
          sourceHash(path) != record.sourceHash)
        return false;
    }
  }
  return true;
}

AssetRecord baseRecord(const AssetId id, const AssetType type,
                       std::filesystem::path sourcePath,
                       const ContentHash source, const ContentHash settingsHash,
                       const SceneImporter &importer,
                       const std::string &target) {
  AssetRecord record;
  record.id = id;
  record.type = type;
    switch (type) {
    case AssetType::Texture:
    record.artifactSchemaVersion = TextureArtifact::kSchemaVersion;
    break;
  case AssetType::Mesh:
    record.artifactSchemaVersion =
        ImportedGltfScene::kMeshArtifactSchemaVersion;
    break;
  case AssetType::Material:
    record.artifactSchemaVersion =
        ImportedGltfScene::kMaterialArtifactSchemaVersion;
    break;
  case AssetType::Scene:
    record.artifactSchemaVersion =
        ImportedGltfScene::kSceneArtifactSchemaVersion;
    break;
  default:
    throw std::invalid_argument("Unsupported reference asset type");
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

struct TextureCookWork {
  std::filesystem::path path;
  AssetId id{};
  TextureRole role = TextureRole::BaseColor;
  TextureColorSpace colorSpace = TextureColorSpace::Linear;
  ContentHash source{};
  std::vector<std::byte> sourceBytes;
  TextureArtifact artifact{};
};

void readTextureSource(void *context, JobContext &job) {
  auto &work = *static_cast<TextureCookWork *>(context);
  if (job.cancellationRequested())
    return;
  work.sourceBytes = readSource(work.path);
  work.source = hashBytes(work.sourceBytes);
}

void cookTexture(void *context, JobContext &job) {
  auto &work = *static_cast<TextureCookWork *>(context);
  if (job.cancellationRequested())
    return;
  work.artifact = importTextureArtifact(work.sourceBytes, work.path, work.id,
                                        work.role, work.colorSpace);
}

bool matchesTextureWork(const TextureCookWork &work,
                        const std::filesystem::path &assetRoot,
                        const ImportedTextureReference &texture) {
  return work.id == texture.id && work.path == assetRoot / texture.sourcePath &&
         work.role == texture.role && work.colorSpace == texture.colorSpace;
}

} // namespace

MeshAssetHandle referenceMeshHandle(const ImportedGltfScene &scene,
                                    const AssetId meshId) {
  const auto found =
      std::ranges::find(scene.meshes, meshId, &ImportedMeshPrimitive::id);
  if (found == scene.meshes.end()) {
    throw std::runtime_error(
        "Mesh asset is not present in the reference scene");
  }
  const std::size_t ordinal =
      static_cast<std::size_t>(found - scene.meshes.begin());
  constexpr std::uint32_t kFirstAuthoredMeshIndex =
      builtin_assets::kReferenceMesh.index;
  if (ordinal >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() -
                               kFirstAuthoredMeshIndex)) {
    throw std::runtime_error("Reference scene mesh handle range is exhausted");
  }
  return {kFirstAuthoredMeshIndex + static_cast<std::uint32_t>(ordinal), 1U};
}

ReferenceAssetBundle
cookReferenceAssetsImpl(const std::filesystem::path &assetRoot,
                        const std::filesystem::path &cacheRoot,
                        std::string targetPlatform, JobSystem *jobs) {
  const auto start = std::chrono::steady_clock::now();
  if (targetPlatform.empty())
    throw std::invalid_argument("Asset target platform must not be empty");
  const std::filesystem::path scenePath = assetRoot / "reference_scene.gltf";
  const SceneImporter &importer = referenceImporter();
  const ContentHash sceneHash = sourceHash(scenePath);
  const ContentHash settingsHash = hashString(kSettings);
  DerivedDataCache cache{cacheRoot / "ddc"};
  const std::filesystem::path databasePath =
      cacheRoot / "asset_database.veasdb";

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
  if (std::filesystem::is_regular_file(databasePath, existsError) &&
      !existsError) {
    try {
      AssetDatabase existing = AssetDatabase::load(databasePath);
      if (databaseIsCurrent(existing, assetRoot, sceneHash, settingsHash,
                            importer, targetPlatform)) {
        result.scene = loadBundle(existing, cache);
        result.database = std::move(existing);
        result.metrics.cacheHits =
            static_cast<std::uint32_t>(result.database.records().size());
        result.metrics.cookMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start)
                .count();
        return result;
      }
    } catch (const std::runtime_error &) {
            // The source database is authoritative; cache metadata and artifacts
            // are disposable and are rebuilt transactionally below.
    }
  }

  ImportedGltfScene imported =
      importer.import(scenePath, builtin_assets::kReferenceSceneId);
  std::ranges::sort(imported.meshes, {}, &ImportedMeshPrimitive::id);
  std::ranges::sort(imported.materials, {}, &ImportedMaterial::id);

  std::vector<TextureCookWork> textureWork;
  if (jobs != nullptr) {
    for (const ImportedMaterial &material : imported.materials) {
      for (const ImportedTextureReference &texture : material.textures) {
        if (std::ranges::find_if(textureWork, [&](const TextureCookWork &work) {
              return matchesTextureWork(work, assetRoot, texture);
            }) != textureWork.end()) {
          continue;
        }
        textureWork.push_back({assetRoot / texture.sourcePath,
                               texture.id,
                               texture.role,
                               texture.colorSpace,
                               {},
                               {}});
      }
    }

    std::vector<JobHandle> readJobs;
    std::vector<JobHandle> importJobs;
    readJobs.reserve(textureWork.size());
    importJobs.reserve(textureWork.size());
    std::exception_ptr textureFailure;
    try {
      for (TextureCookWork &work : textureWork) {
        readJobs.push_back(jobs->submit({.name = "texture-source-read",
                                         .callback = readTextureSource,
                                         .context = &work,
                                         .category = JobCategory::Io}));
      }
      for (std::size_t index = 0; index < textureWork.size(); ++index) {
        const std::array dependency{readJobs[index]};
        importJobs.push_back(jobs->submit({.name = "asset-texture-import",
                                           .callback = cookTexture,
                                           .context = &textureWork[index],
                                           .dependencies = dependency,
                                           .category = JobCategory::Asset}));
      }
    } catch (...) {
      textureFailure = std::current_exception();
    }
    try {
      jobs->waitAll(importJobs);
    } catch (...) {
      if (!textureFailure)
        textureFailure = std::current_exception();
    }
    try {
      jobs->waitAll(readJobs);
    } catch (...) {
      if (!textureFailure)
        textureFailure = std::current_exception();
    }
    jobs->releaseAll(importJobs);
    jobs->releaseAll(readJobs);
    if (textureFailure)
      std::rethrow_exception(textureFailure);
  }
  std::vector<AssetRecord> records;
  records.reserve(imported.meshes.size() + imported.materials.size() * 2U + 1U);

  for (const ImportedMaterial &material : imported.materials) {
    for (const ImportedTextureReference &texture : material.textures) {
      const std::filesystem::path textureSourcePath = texture.sourcePath;
      ContentHash textureHash{};
      TextureArtifact sequentialTexture{};
      const TextureArtifact *importedTexture = nullptr;
      if (jobs != nullptr) {
        const auto found =
            std::ranges::find_if(textureWork, [&](const TextureCookWork &work) {
              return matchesTextureWork(work, assetRoot, texture);
            });
        if (found == textureWork.end()) {
          throw std::runtime_error("Parallel texture cook result is missing");
        }
        textureHash = found->source;
        importedTexture = &found->artifact;
      } else {
        const std::filesystem::path path = assetRoot / textureSourcePath;
        const std::vector<std::byte> source = readSource(path);
        textureHash = hashBytes(source);
        sequentialTexture = importTextureArtifact(
            source, path, texture.id, texture.role, texture.colorSpace);
        importedTexture = &sequentialTexture;
      }
      AssetRecord textureRecord =
          baseRecord(texture.id, AssetType::Texture, textureSourcePath,
                     textureHash, settingsHash, importer, targetPlatform);
      const std::vector<std::byte> texturePayload =
          serializeTextureArtifact(*importedTexture);
      textureRecord.artifactKey =
          keyFor(textureRecord, hashBytes(texturePayload),
                 ArtifactType::Texture, {}, targetPlatform);
      accountPublication(
          cache.publish(textureRecord.artifactKey, ArtifactType::Texture,
                        textureRecord.artifactSchemaVersion, texturePayload));
      textureRecord.artifactPath =
          cache.artifactPath(textureRecord.artifactKey, ArtifactType::Texture)
              .lexically_relative(cacheRoot);
      records.push_back(std::move(textureRecord));
    }
  }
  std::ranges::sort(records, {}, &AssetRecord::id);
  records.erase(
      std::unique(records.begin(), records.end(),
                  [](const AssetRecord &left, const AssetRecord &right) {
                    return left.id == right.id;
                  }),
      records.end());

  for (const ImportedMaterial &material : imported.materials) {
    AssetRecord materialRecord =
        baseRecord(material.id, AssetType::Material, "reference_scene.gltf",
                   sceneHash, settingsHash, importer, targetPlatform);
    std::vector<ContentHash> dependencyHashes;
    for (const ImportedTextureReference &texture : material.textures) {
      materialRecord.dependencies.push_back(texture.id);
      const auto found =
          std::ranges::find(records, texture.id, &AssetRecord::id);
      if (found == records.end())
        throw std::runtime_error("Imported material texture record is missing");
      dependencyHashes.push_back(found->artifactKey);
    }
    const std::vector<std::byte> payload = serializeMaterialArtifact(material);
    materialRecord.artifactKey =
        keyFor(materialRecord, hashBytes(payload), ArtifactType::Material,
               dependencyHashes, targetPlatform);
    accountPublication(
        cache.publish(materialRecord.artifactKey, ArtifactType::Material,
                      materialRecord.artifactSchemaVersion, payload));
    materialRecord.artifactPath =
        cache.artifactPath(materialRecord.artifactKey, ArtifactType::Material)
            .lexically_relative(cacheRoot);
    records.push_back(std::move(materialRecord));
  }

  for (const ImportedMeshPrimitive &mesh : imported.meshes) {
    AssetRecord meshRecord =
        baseRecord(mesh.id, AssetType::Mesh, "reference_scene.gltf", sceneHash,
                   settingsHash, importer, targetPlatform);
    std::vector<ContentHash> dependencyHashes;
    if (mesh.material.valid()) {
      meshRecord.dependencies.push_back(mesh.material);
      const auto found =
          std::ranges::find(records, mesh.material, &AssetRecord::id);
      if (found == records.end())
        throw std::runtime_error("Imported mesh material record is missing");
      dependencyHashes.push_back(found->artifactKey);
    }
    const std::vector<std::byte> payload = serializeMeshArtifact(mesh);
    meshRecord.artifactKey =
        keyFor(meshRecord, hashBytes(payload), ArtifactType::Mesh,
               dependencyHashes, targetPlatform);
    accountPublication(cache.publish(meshRecord.artifactKey, ArtifactType::Mesh,
                                     meshRecord.artifactSchemaVersion,
                                     payload));
    meshRecord.artifactPath =
        cache.artifactPath(meshRecord.artifactKey, ArtifactType::Mesh)
            .lexically_relative(cacheRoot);
    records.push_back(std::move(meshRecord));
  }

  AssetRecord sceneRecord =
      baseRecord(imported.sceneId, AssetType::Scene, "reference_scene.gltf",
                 sceneHash, settingsHash, importer, targetPlatform);
  std::vector<ContentHash> sceneDependencies;
  for (const AssetRecord &record : records) {
    if (record.type == AssetType::Mesh || record.type == AssetType::Material) {
            sceneRecord.dependencies.push_back(record.id);
            sceneDependencies.push_back(record.artifactKey);
        }
  }
  std::ranges::sort(sceneRecord.dependencies);
  const std::vector<std::byte> scenePayload = serializeSceneArtifact(imported);
  sceneRecord.artifactKey =
      keyFor(sceneRecord, hashBytes(scenePayload), ArtifactType::Scene,
             sceneDependencies, targetPlatform);
  accountPublication(cache.publish(sceneRecord.artifactKey, ArtifactType::Scene,
                                   sceneRecord.artifactSchemaVersion,
                                   scenePayload));
  sceneRecord.artifactPath =
      cache.artifactPath(sceneRecord.artifactKey, ArtifactType::Scene)
          .lexically_relative(cacheRoot);
  records.push_back(std::move(sceneRecord));

    AssetDatabase replacement;
    replacement.replaceAll(std::move(records));
    std::filesystem::create_directories(cacheRoot);
  replacement.saveAtomic(databasePath);
  result.database = std::move(replacement);
  result.scene = imported;
  result.metrics.cookMilliseconds =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start)
          .count();
  return result;
}

ReferenceAssetBundle cookReferenceAssets(const std::filesystem::path &assetRoot,
                                         const std::filesystem::path &cacheRoot,
                                         std::string targetPlatform) {
  return cookReferenceAssetsImpl(assetRoot, cacheRoot,
                                 std::move(targetPlatform), nullptr);
}

ReferenceAssetCookTask::ReferenceAssetCookTask(JobSystem &jobs,
                                               std::filesystem::path assetRoot,
                                               std::filesystem::path cacheRoot,
                                               std::string targetPlatform)
    : jobs_(&jobs), assetRoot_(std::move(assetRoot)),
      cacheRoot_(std::move(cacheRoot)),
      targetPlatform_(std::move(targetPlatform)) {
  handle_ = jobs_->submit({.name = "reference-asset-cook",
                           .callback = cook,
                           .context = this,
                           .category = JobCategory::Asset});
}

ReferenceAssetCookTask::~ReferenceAssetCookTask() {
  if (!handle_.valid())
    return;
  try {
    static_cast<void>(jobs_->cancel(handle_));
    static_cast<void>(jobs_->wait(handle_));
  } catch (...) {
  }
  try {
    jobs_->release(handle_);
  } catch (...) {
  }
}

void ReferenceAssetCookTask::cook(void *context, JobContext &job) {
  auto &task = *static_cast<ReferenceAssetCookTask *>(context);
  if (job.cancellationRequested())
    return;
  task.result_.emplace(cookReferenceAssetsImpl(
      task.assetRoot_, task.cacheRoot_, task.targetPlatform_, task.jobs_));
}

ReferenceAssetBundle ReferenceAssetCookTask::take() {
  if (!handle_.valid()) {
    throw std::logic_error("Reference asset cook result was already consumed");
  }
  try {
    const JobStatus status = jobs_->wait(handle_);
    if (status != JobStatus::Succeeded || !result_.has_value()) {
      throw std::runtime_error("Reference asset cook was cancelled");
    }
  } catch (...) {
    jobs_->release(handle_);
    handle_ = {};
    throw;
  }
  jobs_->release(handle_);
  handle_ = {};
  return std::move(*result_);
}

bool ReferenceAssetCookTask::finished() const {
  if (!handle_.valid())
    return true;
  const JobStatus current = jobs_->status(handle_);
  return current == JobStatus::Succeeded || current == JobStatus::Failed ||
         current == JobStatus::Cancelled;
}
struct ReferenceAssetReloader::AsyncReload {
  AsyncReload(JobSystem &jobs, const std::filesystem::path &assetRoot,
              const std::filesystem::path &cacheRoot,
              const std::string &targetPlatform)
      : cook(jobs, assetRoot, cacheRoot, targetPlatform) {}

  ReferenceAssetCookTask cook;
};

ReferenceAssetReloader::ReferenceAssetReloader(std::filesystem::path assetRoot,
                                               std::filesystem::path cacheRoot,
                                               std::string targetPlatform)
    : assetRoot_(std::move(assetRoot)), cacheRoot_(std::move(cacheRoot)),
      targetPlatform_(std::move(targetPlatform)),
      active_(cookReferenceAssets(assetRoot_, cacheRoot_, targetPlatform_)) {}

ReferenceAssetReloader::~ReferenceAssetReloader() = default;

bool ReferenceAssetReloader::beginReload(JobSystem &jobs) {
  if (pendingReload_ != nullptr)
    return false;
  pendingReload_ = std::make_unique<AsyncReload>(jobs, assetRoot_, cacheRoot_,
                                                 targetPlatform_);
  return true;
}

bool ReferenceAssetReloader::reloadPending() const noexcept {
  return pendingReload_ != nullptr;
}

std::optional<AssetReloadResult> ReferenceAssetReloader::pollReload() {
  if (pendingReload_ == nullptr || !pendingReload_->cook.finished()) {
    return std::nullopt;
  }

  AssetReloadResult result;
  try {
    ReferenceAssetBundle candidate = pendingReload_->cook.take();
    result.metrics = candidate.metrics;
    if (candidate.database.serialize() == active_.database.serialize()) {
      result.status = AssetReloadStatus::Unchanged;
    } else {
      static_assert(std::is_nothrow_move_assignable_v<ReferenceAssetBundle>);
      active_ = std::move(candidate);
      ++generation_;
      result.status = AssetReloadStatus::Published;
    }
  } catch (const std::exception &error) {
    result.status = AssetReloadStatus::Failed;
    result.diagnostic =
        "Asynchronous asset reload failed; source=" +
        (assetRoot_ / "reference_scene.gltf").generic_string() +
        "; importer=" + referenceImporter().id + "@" +
        std::to_string(referenceImporter().version) +
        "; cache=" + cacheRoot_.generic_string() +
        "; dependency_chain=scene->material->texture; reason=" + error.what();
  } catch (...) {
    result.status = AssetReloadStatus::Failed;
    result.diagnostic =
        "Asynchronous asset reload failed with a non-standard exception";
  }
  pendingReload_.reset();
  return result;
}

AssetReloadResult ReferenceAssetReloader::reload() noexcept {
  AssetReloadResult result;
  if (pendingReload_ != nullptr) {
    result.status = AssetReloadStatus::Failed;
    result.diagnostic = "Synchronous asset reload rejected while asynchronous "
                        "reload is pending";
    return result;
  }
  try {
    const std::vector<std::byte> previousManifest =
        active_.database.serialize();
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
  } catch (const std::exception &error) {
    result.status = AssetReloadStatus::Failed;
    result.diagnostic =
        "Asset reload failed; source=" +
        (assetRoot_ / "reference_scene.gltf").generic_string() +
        "; importer=" + referenceImporter().id + "@" +
        std::to_string(referenceImporter().version) +
        "; cache=" + cacheRoot_.generic_string() +
        "; dependency_chain=scene->material->texture; reason=" + error.what();
    return result;
  } catch (...) {
    result.status = AssetReloadStatus::Failed;
    result.diagnostic =
        "Asset reload failed with a non-standard exception; source=" +
        (assetRoot_ / "reference_scene.gltf").generic_string() +
        "; importer=" + referenceImporter().id + "@" +
        std::to_string(referenceImporter().version) +
        "; cache=" + cacheRoot_.generic_string();
    return result;
  }
}

} // namespace ve
