#include "assets/AssetDatabase.hpp"
#include "assets/DerivedDataCache.hpp"
#include "assets/GltfImporter.hpp"
#include "assets/RuntimeAssets.hpp"
#include "assets/ReferenceAssetPipeline.hpp"
#include "core/FileSystem.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

template <typename F>
bool throwsRuntimeError(F&& function) {
    try { function(); return false; } catch (const std::runtime_error&) { return true; }
}

ve::AssetRecord record(const ve::AssetId id, const ve::AssetType type, const std::string& path,
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

} // namespace

int main() {
    assert(ve::hashString("").hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(ve::hashString("abc").hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const ve::AssetId texture = ve::AssetId::fromHex("11111111111111112222222222222222");
    const ve::AssetId material = ve::AssetId::fromHex("33333333333333334444444444444444");
    const ve::AssetId scene = ve::AssetId::fromHex("55555555555555556666666666666666");
    assert(ve::AssetId::fromHex(texture.hex()) == texture);
    assert(ve::AssetId::derive(scene, "mesh/0") == ve::AssetId::derive(scene, "mesh/0"));
    assert(ve::AssetId::derive(scene, "mesh/0") != ve::AssetId::derive(scene, "mesh/1"));

    ve::AssetDatabase database;
    database.replaceAll({record(scene, ve::AssetType::Scene, "scene/reference.gltf", {material}),
                         record(texture, ve::AssetType::Texture, "textures/base.png"),
                         record(material, ve::AssetType::Material, "scene/reference.gltf", {texture})});
    assert(database.generation() == 1U);
    assert(database.records().front().id == texture);
    const std::vector<std::byte> serialized = database.serialize();
    const ve::AssetDatabase restored = ve::AssetDatabase::deserialize(serialized);
    assert(restored.generation() == database.generation());
    assert(restored.records().size() == 3U);
    assert(restored.find(material)->dependencies == std::vector<ve::AssetId>{texture});

    ve::AssetRecord movedTexture = *database.find(texture);
    movedTexture.sourcePath = "relocated/base.png";
    database.upsert(movedTexture);
    assert(database.find(texture)->sourcePath == std::filesystem::path{"relocated/base.png"});
    const std::uint64_t beforeFailedTransaction = database.generation();
    assert(throwsRuntimeError([&] {
        database.upsert(record(ve::AssetId::fromHex("77777777777777778888888888888888"),
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
        database.replaceAll({record(texture, ve::AssetType::Texture, "a", {material}),
                             record(material, ve::AssetType::Material, "b", {texture})});
    }));
    struct TestRuntimeAssetTag;
    ve::RuntimeAssetRegistry<std::string, TestRuntimeAssetTag> runtimeAssets;
    const ve::AssetId runtimeId = ve::AssetId::derive(scene, "runtime/test");
    const auto firstRuntimeHandle = runtimeAssets.request(runtimeId);
    assert(runtimeAssets.request(runtimeId) == firstRuntimeHandle);
    assert(runtimeAssets.state(firstRuntimeHandle) == ve::RuntimeAssetState::Loading);
    runtimeAssets.fail(firstRuntimeHandle, "injected load failure");
    assert(runtimeAssets.state(firstRuntimeHandle) == ve::RuntimeAssetState::Failed);
    assert(runtimeAssets.resolve(firstRuntimeHandle) == nullptr);
    runtimeAssets.retry(firstRuntimeHandle);
    runtimeAssets.publish(firstRuntimeHandle, "ready");
    assert(*runtimeAssets.resolve(firstRuntimeHandle) == "ready");
    runtimeAssets.publish(firstRuntimeHandle, "reloaded");
    assert(*runtimeAssets.resolve(firstRuntimeHandle) == "reloaded");
    runtimeAssets.retire(firstRuntimeHandle);
    runtimeAssets.release(firstRuntimeHandle);
    assert(runtimeAssets.resolve(firstRuntimeHandle) == nullptr);
    const auto reusedRuntimeHandle = runtimeAssets.request(ve::AssetId::derive(scene, "runtime/reused"));
    assert(reusedRuntimeHandle.index == firstRuntimeHandle.index);
    assert(reusedRuntimeHandle.generation != firstRuntimeHandle.generation);

    std::vector<std::byte> malformed = serialized;
    const std::filesystem::path referenceScene =
        std::filesystem::path{VOLKENGINE_TEST_ASSET_DIR} / "reference_scene.gltf";
    const ve::ImportedGltfScene imported = ve::importGltfScene(referenceScene, scene);
    assert(imported.nodes.size() == 2U);
    assert(imported.nodes[1].parent == 0U);
    assert(imported.meshes.size() == 1U);
    assert(imported.meshes[0].mesh.vertices.size() == 3U);
    assert(imported.meshes[0].mesh.indices == std::vector<std::uint32_t>({0U, 1U, 2U}));
    assert(imported.meshes[0].mesh.bounds.valid);
    assert(imported.materials.size() == 1U);
    assert(imported.materials[0].textures.size() == 3U);
    assert(imported.materials[0].textures[0].role == ve::TextureRole::BaseColor);
    assert(imported.materials[0].textures[0].colorSpace == ve::TextureColorSpace::Srgb);
    assert(imported.materials[0].textures[1].role == ve::TextureRole::MetallicRoughness);
    assert(imported.materials[0].textures[1].colorSpace == ve::TextureColorSpace::Linear);
    assert(imported.materials[0].textures[2].role == ve::TextureRole::Normal);
    assert(imported.materials[0].textures[2].colorSpace == ve::TextureColorSpace::Linear);
    const std::vector<std::byte> meshArtifact = ve::serializeMeshArtifact(imported.meshes[0]);
    const ve::ImportedMeshPrimitive meshRoundTrip = ve::deserializeMeshArtifact(meshArtifact);
    assert(meshRoundTrip.id == imported.meshes[0].id);
    assert(meshRoundTrip.mesh.vertices.size() == imported.meshes[0].mesh.vertices.size());
    const std::vector<std::byte> materialArtifact = ve::serializeMaterialArtifact(imported.materials[0]);
    const ve::ImportedMaterial materialRoundTrip = ve::deserializeMaterialArtifact(materialArtifact);
    assert(materialRoundTrip.id == imported.materials[0].id);
    assert(materialRoundTrip.textures[0].colorSpace == ve::TextureColorSpace::Srgb);
    const std::vector<std::byte> sceneArtifact = ve::serializeSceneArtifact(imported);
    const ve::ImportedGltfScene sceneRoundTrip = ve::deserializeSceneArtifact(sceneArtifact);
    assert(sceneRoundTrip.sceneId == imported.sceneId);
    assert(sceneRoundTrip.nodes.size() == imported.nodes.size());
    std::vector<std::byte> truncatedMesh = meshArtifact;
    truncatedMesh.pop_back();
    assert(throwsRuntimeError([&] {
        static_cast<void>(ve::deserializeMeshArtifact(truncatedMesh));
    }));

    malformed.resize(12U);
    assert(throwsRuntimeError([&] { static_cast<void>(ve::AssetDatabase::deserialize(malformed)); }));
    malformed = serialized;
    malformed[8U] = std::byte{99};
    assert(throwsRuntimeError([&] { static_cast<void>(ve::AssetDatabase::deserialize(malformed)); }));

    const std::filesystem::path temp = std::filesystem::temp_directory_path() /
        ("volkengine-asset-pipeline-" + std::to_string(reinterpret_cast<std::uintptr_t>(&database)));
    std::error_code error;
    std::filesystem::remove_all(temp, error);
    const std::filesystem::path assetRoot{VOLKENGINE_TEST_ASSET_DIR};
    const ve::ReferenceAssetBundle coldCook =
        ve::cookReferenceAssets(assetRoot, temp / "cook-a", "test-x64");
    assert(coldCook.metrics.cacheMisses == coldCook.database.records().size());
    assert(coldCook.metrics.rebuiltAssets == coldCook.database.records().size());
    assert(coldCook.scene.meshes.size() == 1U);
    const std::vector<std::byte> firstManifest = coldCook.database.serialize();
    const ve::ReferenceAssetBundle warmCook =
        ve::cookReferenceAssets(assetRoot, temp / "cook-a", "test-x64");
    assert(warmCook.metrics.cacheHits == warmCook.database.records().size());
    assert(warmCook.metrics.cacheMisses == 0U);
    assert(warmCook.database.serialize() == firstManifest);
    const std::array corruptManifest{std::byte{0x01}, std::byte{0x02}};
    ve::writeBinaryFileAtomic(temp / "cook-a" / "asset_database.veasdb", corruptManifest);
    const ve::ReferenceAssetBundle recoveredCook =
        ve::cookReferenceAssets(assetRoot, temp / "cook-a", "test-x64");
    assert(recoveredCook.metrics.cacheHits == recoveredCook.database.records().size());
    assert(recoveredCook.metrics.cacheMisses == 0U);
    assert(recoveredCook.database.serialize() == firstManifest);
    const ve::ReferenceAssetBundle independentCook =
        ve::cookReferenceAssets(assetRoot, temp / "cook-b", "test-x64");
    assert(independentCook.database.serialize() == firstManifest);
    const std::filesystem::path incrementalRoot = temp / "incremental-source";
    std::filesystem::create_directories(incrementalRoot / "textures");
    constexpr std::array<std::string_view, 4> sourceFiles{
        "reference_scene.gltf", "textures/ground_albedo.png",
        "textures/ground_normal.png", "textures/ground_orm.png"};
    for (const std::string_view sourceFile : sourceFiles) {
        ve::writeBinaryFileAtomic(incrementalRoot / sourceFile,
                                  ve::readBinaryFile(assetRoot / sourceFile));
    }
    const ve::ReferenceAssetBundle incrementalCold =
        ve::cookReferenceAssets(incrementalRoot, temp / "incremental-cache", "test-x64");
    assert(incrementalCold.metrics.cacheMisses == incrementalCold.database.records().size());
    std::vector<std::byte> changedAlbedo =
        ve::readBinaryFile(incrementalRoot / "textures/ground_albedo.png");
    changedAlbedo.push_back(std::byte{0x00});
    ve::writeBinaryFileAtomic(incrementalRoot / "textures/ground_albedo.png",
                              changedAlbedo);
    const ve::ReferenceAssetBundle incrementalReload =
        ve::cookReferenceAssets(incrementalRoot, temp / "incremental-cache", "test-x64");
    assert(incrementalReload.metrics.cacheMisses == 4U);
    ve::ReferenceAssetReloader reloader{
        incrementalRoot, temp / "incremental-cache", "test-x64"};
    const std::vector<std::byte> activeBeforeFailure =
        reloader.active().database.serialize();
    const std::array corruptScene{std::byte{'{'}, std::byte{0x00}};
    ve::writeBinaryFileAtomic(incrementalRoot / "reference_scene.gltf", corruptScene);
    ve::writeBinaryFileAtomic(incrementalRoot / "textures/ground_albedo.png",
                              ve::readBinaryFile(assetRoot / "textures/ground_albedo.png"));
    const ve::AssetReloadResult failedReload = reloader.reload();
    assert(failedReload.status == ve::AssetReloadStatus::Failed);
    assert(failedReload.diagnostic.find("volkengine.cgltf@1") != std::string::npos);
    assert(failedReload.diagnostic.find("dependency_chain") != std::string::npos);
    assert(reloader.generation() == 1U);
    assert(reloader.active().database.serialize() == activeBeforeFailure);
    ve::writeBinaryFileAtomic(incrementalRoot / "reference_scene.gltf",
                              ve::readBinaryFile(assetRoot / "reference_scene.gltf"));
    const ve::AssetReloadResult recoveredReload = reloader.reload();
    assert(recoveredReload.status == ve::AssetReloadStatus::Published);
    assert(recoveredReload.metrics.rebuiltAssets == 0U);
    assert(recoveredReload.metrics.cacheHits ==
           reloader.active().database.records().size());
    assert(reloader.generation() == 2U);
    assert(reloader.reload().status == ve::AssetReloadStatus::Unchanged);
    assert(incrementalReload.metrics.rebuiltAssets == 4U);
    assert(incrementalReload.metrics.cacheHits == 2U);
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
    const auto payload = std::as_bytes(std::span{payloadText.data(), payloadText.size()});
    assert(cache.publish(key, ve::ArtifactType::Mesh, 1, payload));
    assert(!cache.publish(key, ve::ArtifactType::Mesh, 1, payload));
    const ve::ArtifactBlob artifact = cache.load(key, ve::ArtifactType::Mesh, 1);
    assert(artifact.payload == std::vector<std::byte>(payload.begin(), payload.end()));
    assert(throwsRuntimeError([&] { static_cast<void>(cache.load(key, ve::ArtifactType::Mesh, 2)); }));

    std::vector<std::byte> corrupt = ve::readBinaryFile(cache.artifactPath(key, ve::ArtifactType::Mesh));
    corrupt.back() ^= std::byte{1};
    ve::writeBinaryFileAtomic(cache.artifactPath(key, ve::ArtifactType::Mesh), corrupt);
    assert(throwsRuntimeError([&] { static_cast<void>(cache.load(key, ve::ArtifactType::Mesh, 1)); }));
    std::filesystem::remove_all(temp, error);
    return 0;
}
