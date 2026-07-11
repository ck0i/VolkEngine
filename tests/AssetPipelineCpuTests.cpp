#include "assets/AssetDatabase.hpp"
#include "assets/DerivedDataCache.hpp"
#include "assets/GltfImporter.hpp"
#include "assets/ReferenceAssetPipeline.hpp"
#include "assets/RuntimeAssets.hpp"
#include "assets/SceneImporter.hpp"
#include "assets/TextureArtifact.hpp"
#include "core/FileSystem.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

template <typename F> bool throwsRuntimeError(F &&function) {
  try {
    function();
    return false;
  } catch (const std::runtime_error &) {
    return true;
  }
}

template <typename F> bool throwsInvalidArgument(F &&function) {
  try {
    function();
    return false;
  } catch (const std::invalid_argument &) {
    return true;
  }
}

ve::AssetRecord record(const ve::AssetId id, const ve::AssetType type,
                       const std::string &path,
                       std::vector<ve::AssetId> dependencies = {}) {
  ve::AssetRecord value;
  value.id = id;
    value.type = type;
    value.sourcePath = path;
    value.sourceHash = ve::hashString(path);
    value.importerId = "test.importer";
    value.importerVersion = 3;
    value.normalizedSettings = "quality=production";
    value.settingsHash = ve::hashString(value.normalizedSettings);
    value.dependencies = std::move(dependencies);
    value.artifactKey = ve::hashString(id.hex());
    value.artifactPath = "artifacts/test.veart";
    value.target = "test-x64";
    value.state = ve::AssetState::Ready;
    return value;
}
template <typename T>
void appendLittle(std::vector<std::byte> &bytes, const T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bytes.push_back(static_cast<std::byte>(static_cast<std::uint64_t>(value) >>
                                           (index * 8U)));
  }
}

std::vector<std::byte> makeBc1Ktx2(const std::uint32_t supercompression = 0U,
                                   const bool overlappingMetadata = false) {
  constexpr std::array<std::byte, 12> identifier{
      std::byte{0xAB}, std::byte{'K'},  std::byte{'T'},  std::byte{'X'},
      std::byte{' '},  std::byte{'2'},  std::byte{'0'},  std::byte{0xBB},
      std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
  std::vector<std::byte> bytes(identifier.begin(), identifier.end());
  appendLittle<std::uint32_t>(bytes, 134U);
  appendLittle<std::uint32_t>(bytes, 1U);
    appendLittle<std::uint32_t>(bytes, 4U);
    appendLittle<std::uint32_t>(bytes, 4U);
    appendLittle<std::uint32_t>(bytes, 0U);
    appendLittle<std::uint32_t>(bytes, 0U);
    appendLittle<std::uint32_t>(bytes, 1U);
    appendLittle<std::uint32_t>(bytes, 1U);
    appendLittle<std::uint32_t>(bytes, supercompression);
    appendLittle<std::uint32_t>(bytes, 0U);
    appendLittle<std::uint32_t>(bytes, 0U);
    appendLittle<std::uint32_t>(bytes, overlappingMetadata ? 104U : 0U);
    appendLittle<std::uint32_t>(bytes, overlappingMetadata ? 4U : 0U);
    appendLittle<std::uint64_t>(bytes, 0U);
    appendLittle<std::uint64_t>(bytes, 0U);
    appendLittle<std::uint64_t>(bytes, 104U);
    appendLittle<std::uint64_t>(bytes, 8U);
    appendLittle<std::uint64_t>(bytes, 8U);
    constexpr std::array<std::byte, 8> block{
        std::byte{0x00}, std::byte{0xF8}, std::byte{0xE0}, std::byte{0x07},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    bytes.insert(bytes.end(), block.begin(), block.end());
  return bytes;
}

} // namespace

int main() {
  assert(ve::hashString("").hex() ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(ve::hashString("abc").hex() ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const ve::AssetId texture =
      ve::AssetId::fromHex("11111111111111112222222222222222");
  const ve::AssetId material =
      ve::AssetId::fromHex("33333333333333334444444444444444");
  const ve::AssetId scene =
      ve::AssetId::fromHex("55555555555555556666666666666666");
  assert(ve::AssetId::fromHex(texture.hex()) == texture);
  assert(ve::AssetId::derive(scene, "mesh/0") ==
         ve::AssetId::derive(scene, "mesh/0"));
  assert(ve::AssetId::derive(scene, "mesh/0") !=
         ve::AssetId::derive(scene, "mesh/1"));

  ve::SceneImporterRegistry importers;
  ve::registerGltfImporter(importers);
  assert(importers.importers().size() == 1U);
  assert(importers.importerFor("scene.GLTF").id == "volkengine.cgltf");
  assert(importers.importerFor("scene.glb").version == 2U);
  assert(throwsRuntimeError(
      [&] { static_cast<void>(importers.importerFor("scene.fbx")); }));
  assert(throwsInvalidArgument([&] { ve::registerGltfImporter(importers); }));

  ve::AssetDatabase database;
  database.replaceAll(
      {record(scene, ve::AssetType::Scene, "scene/reference.gltf", {material}),
       record(texture, ve::AssetType::Texture, "textures/base.png"),
       record(material, ve::AssetType::Material, "scene/reference.gltf",
              {texture})});
  assert(database.generation() == 1U);
  assert(database.records().front().id == texture);
  const std::vector<std::byte> serialized = database.serialize();
  const ve::AssetDatabase restored = ve::AssetDatabase::deserialize(serialized);
  assert(restored.generation() == database.generation());
  assert(restored.records().size() == 3U);
  assert(restored.find(material)->dependencies ==
         std::vector<ve::AssetId>{texture});

  ve::AssetRecord movedTexture = *database.find(texture);
  movedTexture.sourcePath = "relocated/base.png";
  database.upsert(movedTexture);
  assert(database.find(texture)->sourcePath ==
         std::filesystem::path{"relocated/base.png"});
  const std::uint64_t beforeFailedTransaction = database.generation();
  assert(throwsRuntimeError([&] {
    database.upsert(
        record(ve::AssetId::fromHex("77777777777777778888888888888888"),
               ve::AssetType::Material, "bad.material",
               {ve::AssetId::fromHex("9999999999999999aaaaaaaaaaaaaaaa")}));
  }));
    assert(database.generation() == beforeFailedTransaction);
    assert(database.records().size() == 3U);

    database.markChanged(texture, "source content hash changed");
    assert(database.find(texture)->state == ve::AssetState::Stale);
    assert(database.find(material)->state == ve::AssetState::Stale);
    assert(database.find(scene)->state == ve::AssetState::Stale);
    assert(database.find(material)->diagnostic == "source content hash changed");

    assert(throwsRuntimeError([&] {
        database.replaceAll({record(texture, ve::AssetType::Texture, "a"),
                         record(texture, ve::AssetType::Texture, "b")});
  }));
  assert(throwsRuntimeError([&] {
    database.replaceAll(
        {record(texture, ve::AssetType::Texture, "a", {material}),
         record(material, ve::AssetType::Material, "b", {texture})});
  }));
  struct TestRuntimeAssetTag;
    ve::RuntimeAssetRegistry<std::string, TestRuntimeAssetTag> runtimeAssets;
  const ve::AssetId runtimeId = ve::AssetId::derive(scene, "runtime/test");
  const auto firstRuntimeHandle = runtimeAssets.request(runtimeId);
  assert(runtimeAssets.request(runtimeId) == firstRuntimeHandle);
  assert(runtimeAssets.state(firstRuntimeHandle) ==
         ve::RuntimeAssetState::Loading);
  runtimeAssets.fail(firstRuntimeHandle, "injected load failure");
  assert(runtimeAssets.state(firstRuntimeHandle) ==
         ve::RuntimeAssetState::Failed);
  assert(runtimeAssets.resolve(firstRuntimeHandle) == nullptr);
  runtimeAssets.retry(firstRuntimeHandle);
  runtimeAssets.publish(firstRuntimeHandle, "ready");
    assert(*runtimeAssets.resolve(firstRuntimeHandle) == "ready");
    runtimeAssets.publish(firstRuntimeHandle, "reloaded");
    assert(*runtimeAssets.resolve(firstRuntimeHandle) == "reloaded");
  runtimeAssets.retire(firstRuntimeHandle);
  runtimeAssets.release(firstRuntimeHandle);
  assert(runtimeAssets.resolve(firstRuntimeHandle) == nullptr);
  const auto reusedRuntimeHandle =
      runtimeAssets.request(ve::AssetId::derive(scene, "runtime/reused"));
  assert(reusedRuntimeHandle.index == firstRuntimeHandle.index);
  assert(reusedRuntimeHandle.generation != firstRuntimeHandle.generation);

  std::vector<std::byte> malformed = serialized;
  const std::filesystem::path referenceScene =
      std::filesystem::path{VOLKENGINE_TEST_ASSET_DIR} / "reference_scene.gltf";
  const ve::ImportedGltfScene imported =
      ve::importGltfScene(referenceScene, scene);
  assert(imported.nodes.size() == 3U);
  assert(imported.nodes[1].parent == 0U);
  assert(imported.nodes[2].parent == 0U);
  assert(imported.meshes.size() == 2U);
  assert(imported.meshes[0].mesh.vertices.size() == 3U);
  assert(imported.meshes[0].mesh.indices ==
         std::vector<std::uint32_t>({0U, 1U, 2U}));
  assert(imported.meshes[0].mesh.bounds.valid);
  assert(imported.materials.size() == 1U);
  assert(imported.materials[0].textures.size() == 3U);
  assert(imported.materials[0].textures[0].role == ve::TextureRole::BaseColor);
  assert(imported.materials[0].textures[0].colorSpace ==
         ve::TextureColorSpace::Srgb);
  assert(imported.materials[0].textures[1].role ==
         ve::TextureRole::MetallicRoughness);
  assert(imported.materials[0].textures[1].colorSpace ==
         ve::TextureColorSpace::Linear);
  assert(imported.materials[0].textures[2].role == ve::TextureRole::Normal);
  assert(imported.materials[0].textures[2].colorSpace ==
         ve::TextureColorSpace::Linear);
  assert(imported.animations.size() == 1U);
  assert(imported.animations[0].name == "Triangle Move");
  assert(imported.animations[0].duration == 2.0f);
  assert(imported.animations[0].channels.size() == 1U);
  assert(imported.animations[0].channels[0].targetNode == 1U);
  assert(imported.animations[0].channels[0].target ==
         ve::AnimationTarget::Translation);
  assert(imported.animations[0].channels[0].interpolation ==
         ve::AnimationInterpolation::Linear);
  assert(imported.animations[0].channels[0].keyframeCount == 2U);
  assert(imported.animations[0].channels[0].startTime == 0.0f);
  assert(imported.animations[0].channels[0].endTime == 2.0f);
  const std::vector<std::byte> meshArtifact =
      ve::serializeMeshArtifact(imported.meshes[0]);
  const ve::ImportedMeshPrimitive meshRoundTrip =
      ve::deserializeMeshArtifact(meshArtifact);
  assert(meshRoundTrip.id == imported.meshes[0].id);
  assert(meshRoundTrip.mesh.vertices.size() ==
         imported.meshes[0].mesh.vertices.size());
  const std::vector<std::byte> materialArtifact =
      ve::serializeMaterialArtifact(imported.materials[0]);
  const ve::ImportedMaterial materialRoundTrip =
      ve::deserializeMaterialArtifact(materialArtifact);
  assert(materialRoundTrip.id == imported.materials[0].id);
  assert(materialRoundTrip.textures[0].colorSpace ==
         ve::TextureColorSpace::Srgb);
  const std::vector<std::byte> sceneArtifact =
      ve::serializeSceneArtifact(imported);
  const ve::ImportedGltfScene sceneRoundTrip =
      ve::deserializeSceneArtifact(sceneArtifact);
  assert(sceneRoundTrip.sceneId == imported.sceneId);
  assert(sceneRoundTrip.nodes.size() == imported.nodes.size());
  assert(sceneRoundTrip.animations.size() == imported.animations.size());
    assert(sceneRoundTrip.animations[0].id == imported.animations[0].id);
    assert(sceneRoundTrip.animations[0].channels[0].targetNode == 1U);
  assert(sceneRoundTrip.animations[0].channels[0].endTime == 2.0f);
  std::vector<std::byte> truncatedMesh = meshArtifact;
  truncatedMesh.pop_back();
  assert(throwsRuntimeError(
      [&] { static_cast<void>(ve::deserializeMeshArtifact(truncatedMesh)); }));

  malformed.resize(12U);
  assert(throwsRuntimeError(
      [&] { static_cast<void>(ve::AssetDatabase::deserialize(malformed)); }));
  malformed = serialized;
  malformed[8U] = std::byte{99};
  assert(throwsRuntimeError(
      [&] { static_cast<void>(ve::AssetDatabase::deserialize(malformed)); }));

  const std::filesystem::path temp =
      std::filesystem::temp_directory_path() /
      ("volkengine-asset-pipeline-" +
       std::to_string(reinterpret_cast<std::uintptr_t>(&database)));
  std::error_code error;
  std::filesystem::remove_all(temp, error);
  std::filesystem::create_directories(temp);
  const std::vector<std::byte> referenceSource =
      ve::readBinaryFile(referenceScene);
  std::string corruptAnimationSource(
      reinterpret_cast<const char *>(referenceSource.data()),
      referenceSource.size());
  const std::string validAnimationPayload =
      "AAAAAAAAAEAAAMC/AAAAAAAAAAAAAAC/AAAAAAAAAAA=";
  const std::size_t animationPayloadOffset =
        corruptAnimationSource.find(validAnimationPayload);
    assert(animationPayloadOffset != std::string::npos);
  corruptAnimationSource.replace(
      animationPayloadOffset, validAnimationPayload.size(),
      "AAAAAAAAAEAAAMC/AAAAAAAAAAAAAAC/AAAAAAAAwH8=");
  const std::filesystem::path corruptAnimationPath =
      temp / "corrupt-animation.gltf";
  ve::writeBinaryFileAtomic(
      corruptAnimationPath,
      std::as_bytes(std::span{corruptAnimationSource.data(),
                              corruptAnimationSource.size()}));
  assert(throwsRuntimeError([&] {
    static_cast<void>(ve::importGltfScene(corruptAnimationPath, scene));
  }));
    const ve::AssetId hdrId = ve::AssetId::derive(scene, "texture/test-hdr");
    const std::string hdrSource =
        "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n"
        "\x80\x40\x20\x81";
    const std::filesystem::path hdrPath = temp / "test.hdr";
    ve::writeBinaryFileAtomic(
        hdrPath, std::as_bytes(std::span{hdrSource.data(), hdrSource.size()}));
    const ve::TextureArtifact hdr = ve::importTextureArtifact(
        hdrPath, hdrId, ve::TextureRole::Emissive, ve::TextureColorSpace::Linear);
    assert(hdr.storage == ve::TextureStorage::Rgba32Float);
    assert(hdr.width == 1U && hdr.height == 1U);
    assert(hdr.data.size() == 4U * sizeof(float));
  float hdrRed = 0.0f;
  std::memcpy(&hdrRed, hdr.data.data(), sizeof(hdrRed));
  assert(std::fabs(hdrRed - 1.0f) < 0.01f);
  const std::vector<std::byte> hdrArtifactBytes =
      ve::serializeTextureArtifact(hdr);
  assert(ve::deserializeTextureArtifact(hdrArtifactBytes).data == hdr.data);

  const ve::AssetId ktxId = ve::AssetId::derive(scene, "texture/test-ktx");
    const std::filesystem::path ktxPath = temp / "test.ktx2";
    ve::writeBinaryFileAtomic(ktxPath, makeBc1Ktx2());
    const ve::TextureArtifact ktx = ve::importTextureArtifact(
        ktxPath, ktxId, ve::TextureRole::BaseColor, ve::TextureColorSpace::Srgb);
  assert(ktx.storage == ve::TextureStorage::Bc1Rgba);
  assert(ktx.width == 4U && ktx.height == 4U);
  assert(ktx.mips.size() == 1U && ktx.mips[0].size == 8U);
  const std::vector<std::byte> ktxArtifactBytes =
      ve::serializeTextureArtifact(ktx);
  assert(ve::deserializeTextureArtifact(ktxArtifactBytes).data == ktx.data);
  ve::writeBinaryFileAtomic(ktxPath, makeBc1Ktx2(1U));
  assert(throwsRuntimeError([&] {
    static_cast<void>(ve::importTextureArtifact(ktxPath, ktxId,
                                                ve::TextureRole::BaseColor,
                                                ve::TextureColorSpace::Srgb));
  }));
  ve::writeBinaryFileAtomic(ktxPath, makeBc1Ktx2(0U, true));
  assert(throwsRuntimeError([&] {
    static_cast<void>(ve::importTextureArtifact(ktxPath, ktxId,
                                                ve::TextureRole::BaseColor,
                                                ve::TextureColorSpace::Srgb));
  }));
  std::vector<std::byte> unalignedKtx = makeBc1Ktx2();
  unalignedKtx.insert(unalignedKtx.begin() + 104, std::byte{0});
  unalignedKtx[80] = std::byte{105};
  ve::writeBinaryFileAtomic(ktxPath, unalignedKtx);
  assert(throwsRuntimeError([&] {
    static_cast<void>(ve::importTextureArtifact(ktxPath, ktxId,
                                                ve::TextureRole::BaseColor,
                                                ve::TextureColorSpace::Srgb));
  }));
  const std::filesystem::path assetRoot{VOLKENGINE_TEST_ASSET_DIR};
  const ve::ReferenceAssetBundle coldCook =
        ve::cookReferenceAssets(assetRoot, temp / "cook-a", "test-x64");
    assert(coldCook.metrics.cacheMisses == coldCook.database.records().size());
    assert(coldCook.metrics.rebuiltAssets == coldCook.database.records().size());
    assert(coldCook.scene.meshes.size() == 2U);
    const std::vector<std::byte> firstManifest = coldCook.database.serialize();
    const ve::ReferenceAssetBundle warmCook =
        ve::cookReferenceAssets(assetRoot, temp / "cook-a", "test-x64");
    assert(warmCook.metrics.cacheHits == warmCook.database.records().size());
  assert(warmCook.metrics.cacheMisses == 0U);
  assert(warmCook.database.serialize() == firstManifest);
  const std::array corruptManifest{std::byte{0x01}, std::byte{0x02}};
  ve::writeBinaryFileAtomic(temp / "cook-a" / "asset_database.veasdb",
                            corruptManifest);
  const ve::ReferenceAssetBundle recoveredCook =
      ve::cookReferenceAssets(assetRoot, temp / "cook-a", "test-x64");
  assert(recoveredCook.metrics.cacheHits ==
         recoveredCook.database.records().size());
  assert(recoveredCook.metrics.cacheMisses == 0U);
  assert(recoveredCook.database.serialize() == firstManifest);
  const ve::ReferenceAssetBundle independentCook =
      ve::cookReferenceAssets(assetRoot, temp / "cook-b", "test-x64");
  assert(independentCook.database.serialize() == firstManifest);
  ve::JobSystem assetJobs({.workerCount = 4,
                           .maximumJobs = 32,
                           .maximumDependencies = 64,
                           .timelineCapacity = 32});
  ve::ReferenceAssetCookTask asynchronousCook{assetJobs, assetRoot,
                                              temp / "cook-c", "test-x64"};
  const ve::ReferenceAssetBundle asynchronousBundle = asynchronousCook.take();
  assert(asynchronousBundle.database.serialize() == firstManifest);
  const ve::JobSystemStats asynchronousStats = assetJobs.stats();
  assert(asynchronousStats.submitted == 7U);
  assert(asynchronousStats.succeeded == 7U);
  assert(asynchronousStats.failed == 0U);
  assert(asynchronousStats.activeJobs == 0U);
  const std::vector<ve::JobTimelineEvent> assetTimeline = assetJobs.timeline();
  assert(assetTimeline.size() == 7U);
  assert(std::ranges::count_if(
             assetTimeline, [](const ve::JobTimelineEvent &event) {
               return std::string_view{event.name} == "asset-texture-import";
             }) == 3);
  assert(std::ranges::count_if(
             assetTimeline, [](const ve::JobTimelineEvent &event) {
               return std::string_view{event.name} == "texture-source-read";
             }) == 3);
  assert(asynchronousStats.categorySubmitted[static_cast<std::size_t>(
             ve::JobCategory::Io)] == 3U);
  assert(asynchronousStats.categorySubmitted[static_cast<std::size_t>(
             ve::JobCategory::Asset)] == 4U);
  assert(std::ranges::count_if(
             assetTimeline, [](const ve::JobTimelineEvent &event) {
               return std::string_view{event.name} == "reference-asset-cook";
             }) == 1);
  bool duplicateTakeRejected = false;
  try {
    static_cast<void>(asynchronousCook.take());
  } catch (const std::logic_error &) {
    duplicateTakeRejected = true;
  }
  assert(duplicateTakeRejected);
  ve::JobSystem singleWorkerJobs({.workerCount = 1,
                                  .maximumJobs = 16,
                                  .maximumDependencies = 16,
                                  .timelineCapacity = 16});
  ve::ReferenceAssetCookTask singleWorkerCook{
      singleWorkerJobs, assetRoot, temp / "cook-single-worker", "test-x64"};
  const ve::ReferenceAssetBundle singleWorkerBundle = singleWorkerCook.take();
  assert(singleWorkerBundle.database.serialize() == firstManifest);
  const ve::JobSystemStats singleWorkerStats = singleWorkerJobs.stats();
  assert(singleWorkerStats.submitted == 7U);
  assert(singleWorkerStats.succeeded == 7U);
  assert(singleWorkerStats.failed == 0U);
  assert(singleWorkerStats.activeJobs == 0U);
  ve::ReferenceAssetCookTask asynchronousWarmCook{assetJobs, assetRoot,
                                                  temp / "cook-c", "test-x64"};
  const ve::ReferenceAssetBundle asynchronousWarm = asynchronousWarmCook.take();
  assert(asynchronousWarm.metrics.cacheHits ==
         asynchronousWarm.database.records().size());
  assert(asynchronousWarm.metrics.cacheMisses == 0U);
  const std::filesystem::path incrementalRoot = temp / "incremental-source";
  std::filesystem::create_directories(incrementalRoot / "textures");
  constexpr std::array<std::string_view, 4> sourceFiles{
        "reference_scene.gltf", "textures/ground_albedo.png",
        "textures/ground_normal.png", "textures/ground_orm.png"};
    for (const std::string_view sourceFile : sourceFiles) {
    ve::writeBinaryFileAtomic(incrementalRoot / sourceFile,
                              ve::readBinaryFile(assetRoot / sourceFile));
  }
  const ve::ReferenceAssetBundle incrementalCold = ve::cookReferenceAssets(
      incrementalRoot, temp / "incremental-cache", "test-x64");
  assert(incrementalCold.metrics.cacheMisses ==
         incrementalCold.database.records().size());
  std::vector<std::byte> animationRenameBytes =
      ve::readBinaryFile(incrementalRoot / "reference_scene.gltf");
  std::string animationRename(
      reinterpret_cast<const char *>(animationRenameBytes.data()),
      animationRenameBytes.size());
  const std::size_t animationNameOffset = animationRename.find("Triangle Move");
  assert(animationNameOffset != std::string::npos);
  animationRename.replace(animationNameOffset,
                          std::string_view{"Triangle Move"}.size(),
                          "Triangle Shift");
  ve::writeBinaryFileAtomic(
      incrementalRoot / "reference_scene.gltf",
      std::as_bytes(std::span{animationRename.data(), animationRename.size()}));
  const ve::ReferenceAssetBundle animationOnlyReload = ve::cookReferenceAssets(
      incrementalRoot, temp / "incremental-cache", "test-x64");
  assert(animationOnlyReload.metrics.cacheMisses == 1U);
  assert(animationOnlyReload.metrics.rebuiltAssets == 1U);
  assert(animationOnlyReload.metrics.cacheHits + 1U ==
         animationOnlyReload.database.records().size());
  assert(animationOnlyReload.scene.animations[0].name == "Triangle Shift");
  for (const ve::AssetRecord &previous : incrementalCold.database.records()) {
    if (previous.type == ve::AssetType::Scene)
      continue;
    const ve::AssetRecord *current =
        animationOnlyReload.database.find(previous.id);
    assert(current != nullptr);
    assert(current->artifactKey == previous.artifactKey);
  }
  ve::writeBinaryFileAtomic(
      incrementalRoot / "reference_scene.gltf",
      ve::readBinaryFile(assetRoot / "reference_scene.gltf"));
  std::vector<std::byte> changedAlbedo =
      ve::readBinaryFile(incrementalRoot / "textures/ground_albedo.png");
    changedAlbedo.push_back(std::byte{0x00});
  ve::writeBinaryFileAtomic(incrementalRoot / "textures/ground_albedo.png",
                            changedAlbedo);
  const ve::ReferenceAssetBundle equivalentSourceReload =
      ve::cookReferenceAssets(incrementalRoot, temp / "incremental-cache",
                              "test-x64");
  assert(equivalentSourceReload.metrics.cacheMisses == 0U);
  assert(equivalentSourceReload.metrics.cacheHits ==
         equivalentSourceReload.database.records().size());
    std::vector<std::byte> changedDecodedAlbedo =
        ve::readBinaryFile(assetRoot / "textures/ground_albedo.ppm");
  changedDecodedAlbedo.back() ^= std::byte{1};
  ve::writeBinaryFileAtomic(incrementalRoot / "textures/ground_albedo.png",
                            changedDecodedAlbedo);
  const ve::ReferenceAssetBundle incrementalReload = ve::cookReferenceAssets(
      incrementalRoot, temp / "incremental-cache", "test-x64");
  assert(incrementalReload.metrics.cacheMisses == 5U);
  ve::ReferenceAssetReloader reloader{incrementalRoot,
                                      temp / "incremental-cache", "test-x64"};
  const std::vector<std::byte> activeBeforeFailure =
      reloader.active().database.serialize();
  ve::JobSystem reloadJobs({.workerCount = 4,
                            .maximumJobs = 32,
                            .maximumDependencies = 64,
                            .timelineCapacity = 64});
  std::uint64_t pollWork = 0;
  const auto pollUntilComplete = [&]() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (reloader.reloadPending()) {
      if (std::optional<ve::AssetReloadResult> result = reloader.pollReload();
          result.has_value()) {
        return *result;
      }
      ++pollWork;
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::yield();
    }
    throw std::runtime_error("Asynchronous reload ended without a result");
  };
  assert(reloader.beginReload(reloadJobs));
  assert(!reloader.beginReload(reloadJobs));
  assert(reloader.reload().status == ve::AssetReloadStatus::Failed);
  const ve::AssetReloadResult unchangedAsync = pollUntilComplete();
  assert(unchangedAsync.status == ve::AssetReloadStatus::Unchanged);
  assert(reloader.generation() == 1U);
  assert(
      std::filesystem::remove(incrementalRoot / "textures/ground_normal.png"));
  assert(reloader.beginReload(reloadJobs));
  const ve::AssetReloadResult missingSourceReload = pollUntilComplete();
  assert(missingSourceReload.status == ve::AssetReloadStatus::Failed);
  assert(missingSourceReload.diagnostic.find("ground_normal.png") !=
         std::string::npos);
  assert(reloader.generation() == 1U);
  assert(reloader.active().database.serialize() == activeBeforeFailure);
  ve::writeBinaryFileAtomic(
      incrementalRoot / "textures/ground_normal.png",
      ve::readBinaryFile(assetRoot / "textures/ground_normal.png"));
  const std::array corruptScene{std::byte{'{'}, std::byte{0x00}};
  ve::writeBinaryFileAtomic(incrementalRoot / "reference_scene.gltf",
                            corruptScene);
  ve::writeBinaryFileAtomic(
      incrementalRoot / "textures/ground_albedo.png",
      ve::readBinaryFile(assetRoot / "textures/ground_albedo.png"));
  const ve::AssetReloadResult failedReload = reloader.reload();
  assert(failedReload.status == ve::AssetReloadStatus::Failed);
  assert(failedReload.diagnostic.find("volkengine.cgltf@2") !=
         std::string::npos);
  assert(failedReload.diagnostic.find("dependency_chain") != std::string::npos);
  assert(reloader.generation() == 1U);
  assert(reloader.active().database.serialize() == activeBeforeFailure);
  ve::writeBinaryFileAtomic(
      incrementalRoot / "reference_scene.gltf",
      ve::readBinaryFile(assetRoot / "reference_scene.gltf"));
  assert(reloader.beginReload(reloadJobs));
  const ve::AssetReloadResult recoveredReload = pollUntilComplete();
  assert(recoveredReload.status == ve::AssetReloadStatus::Published);
  assert(recoveredReload.metrics.rebuiltAssets == 0U);
  assert(recoveredReload.metrics.cacheHits ==
         reloader.active().database.records().size());
  assert(reloader.generation() == 2U);
  assert(reloader.reload().status == ve::AssetReloadStatus::Unchanged);
  assert(!reloader.reloadPending());
  assert(reloadJobs.stats().activeJobs == 0U);
  static_cast<void>(pollWork);
  assert(incrementalReload.metrics.rebuiltAssets == 5U);
  assert(incrementalReload.metrics.cacheHits ==
         incrementalReload.database.records().size() - 5U);
    ve::DerivedDataCache cache{temp / "cache"};
    ve::DerivedDataKeyInput keyInput;
    keyInput.sourceHash = ve::hashString("mesh source");
    keyInput.importerId = "test.gltf";
    keyInput.importerVersion = 7;
    keyInput.settingsHash = ve::hashString("scale=1");
    keyInput.dependencyArtifactHashes = {ve::hashString("material")};
    keyInput.type = ve::ArtifactType::Mesh;
    keyInput.artifactSchemaVersion = 1;
    keyInput.targetPlatform = "linux-x64";
  keyInput.gpuFormat = "vertex48-index32";
  const ve::ContentHash key = ve::makeDerivedDataKey(keyInput);
  const std::string payloadText = "deterministic cooked mesh";
  const auto payload =
      std::as_bytes(std::span{payloadText.data(), payloadText.size()});
  assert(cache.publish(key, ve::ArtifactType::Mesh, 1, payload));
  assert(!cache.publish(key, ve::ArtifactType::Mesh, 1, payload));
  const ve::ArtifactBlob artifact = cache.load(key, ve::ArtifactType::Mesh, 1);
  assert(artifact.payload ==
         std::vector<std::byte>(payload.begin(), payload.end()));
  assert(throwsRuntimeError(
      [&] { static_cast<void>(cache.load(key, ve::ArtifactType::Mesh, 2)); }));

  std::vector<std::byte> corrupt =
      ve::readBinaryFile(cache.artifactPath(key, ve::ArtifactType::Mesh));
  corrupt.back() ^= std::byte{1};
  ve::writeBinaryFileAtomic(cache.artifactPath(key, ve::ArtifactType::Mesh),
                            corrupt);
  assert(throwsRuntimeError(
      [&] { static_cast<void>(cache.load(key, ve::ArtifactType::Mesh, 1)); }));
  std::filesystem::remove_all(temp, error);
  return 0;
}
