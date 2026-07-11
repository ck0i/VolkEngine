#include "core/BinaryIO.hpp"
#include "core/FileSystem.hpp"
#include "editor/AuthoringCooker.hpp"
#include "editor/EditorSession.hpp"
#include "scene/SceneSchema.generated.hpp"

#include <array>
#include <cassert>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

template <typename Exception, typename Callback>
bool throws(Callback &&callback) {
  try {
    callback();
    return false;
  } catch (const Exception &) {
    return true;
  }
}

void writeString(std::vector<std::byte> &bytes, const std::string_view value) {
  ve::appendLittleEndian(bytes, static_cast<std::uint32_t>(value.size()));
  const auto source = std::as_bytes(std::span{value.data(), value.size()});
  bytes.insert(bytes.end(), source.begin(), source.end());
}

std::vector<std::byte>
oneComponentDocument(const ve::SceneTypeId type, const std::uint32_t version,
                     const std::span<const std::byte> payload) {
  std::vector<std::byte> bytes;
  ve::appendLittleEndian(bytes, std::uint32_t{0x55414556U});
  ve::appendLittleEndian(bytes, std::uint32_t{1U});
  ve::appendLittleEndian(bytes, std::uint64_t{1U});
  ve::appendLittleEndian(bytes, std::uint64_t{7U});
  ve::appendLittleEndian(bytes, std::uint64_t{9U});
  writeString(bytes, "Migrated");
  ve::appendLittleEndian(bytes, std::uint64_t{0U});
  ve::appendLittleEndian(bytes, std::uint64_t{0U});
  ve::appendLittleEndian(bytes, std::uint32_t{1U});
  ve::appendLittleEndian(bytes, type);
  ve::appendLittleEndian(bytes, version);
  ve::appendLittleEndian(bytes, static_cast<std::uint64_t>(payload.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

ve::MeshAssetHandle resolveMesh(void *, const ve::AssetId id) {
  if (!id.valid())
    return {};
  return ve::builtin_assets::kReferenceMesh;
}

ve::RenderMaterial resolveMaterial(void *, const ve::AssetId id) {
  if (!id.valid())
    return {};
  return {
      {1.0F, 1.0F, 1.0F, 0.5F},
      {0.0F, 0.0F, 0.0F, 0.0F},
      {0.0F, static_cast<float>(ve::RenderMaterialClass::Standard), 0.0F, 1.0F},
      {ve::TextureAssetHandle{0U, 1U}, ve::TextureAssetHandle{1U, 1U},
       ve::TextureAssetHandle{2U, 1U}}};
}

ve::MeshBounds resolveBounds(void *, const ve::MeshAssetHandle mesh) {
  return mesh.valid() ? ve::MeshBounds{{}, 1.0F, true} : ve::MeshBounds{};
}

} // namespace

int main() {
  static_assert(ve::stableSceneId("ve.scene.transform") ==
                8432326986207199196ULL);
  static_assert(ve::stableSceneId("ve.scene.transform.translation") ==
                4618265383229782425ULL);
  static_assert(ve::stableSceneId("ve.scene.renderable") ==
                979901426717663976ULL);

  const ve::SceneTypeRegistry generated = ve::generatedSceneTypeRegistry();
  const ve::SceneTypeRegistry explicitRegistry =
      ve::explicitSceneTypeRegistry();
  const std::string externalSource =
      ve::readTextFile(VOLKENGINE_TEST_SCENE_SCHEMA);
  const ve::SceneTypeRegistry external =
      ve::externalSceneTypeRegistry(externalSource);
  assert(generated.bindingManifest() == explicitRegistry.bindingManifest());
  assert(generated.bindingManifest() == external.bindingManifest());
  assert(generated.bindingManifest() ==
         ve::builtinSceneTypeRegistry().bindingManifest());
  assert(throws<std::invalid_argument>([&] {
    ve::SceneTypeRegistry duplicate = ve::explicitSceneTypeRegistry();
    duplicate.registerType(
        ve::makeSceneTypeMetadata("Transform", "ve.scene.transform", 2U, {}));
  }));
  assert(throws<std::runtime_error>([] {
    static_cast<void>(ve::externalSceneTypeRegistry(
        "property|Missing|X|x|Float|0|1|1|System.Single\n"));
  }));

  std::vector<std::byte> integerBytes;
  ve::appendLittleEndian(integerBytes, std::uint32_t{0x12345678U});
  assert((integerBytes ==
          std::vector<std::byte>{std::byte{0x78}, std::byte{0x56},
                                 std::byte{0x34}, std::byte{0x12}}));
  assert(ve::readLittleEndian<std::uint32_t>(integerBytes, 0U) == 0x12345678U);
  const std::vector<std::byte> identityTransform =
      ve::encodeAuthoringTransform({});
  assert(identityTransform.size() == 40U);
  assert(identityTransform[24] == std::byte{0x00} &&
         identityTransform[25] == std::byte{0x00} &&
         identityTransform[26] == std::byte{0x80} &&
         identityTransform[27] == std::byte{0x3f});

  std::vector<std::byte> legacyTransform;
  for (const float value : {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F}) {
    ve::appendLittleEndianFloat(legacyTransform, value);
  }
  ve::editor::AuthoringDocument migrated;
  ve::editor::decodeAuthoringDocument(
      migrated,
      oneComponentDocument(ve::kTransformSceneType, 1U, legacyTransform));
  const auto *migratedComponent =
      migrated.component({7U, 9U}, ve::kTransformSceneType);
  assert(migratedComponent != nullptr && migratedComponent->version == 2U);
  const ve::TransformTRS migratedTransform =
      ve::decodeAuthoringTransform(migratedComponent->payload);
  assert(migratedTransform.translation.x == 1.0F &&
         migratedTransform.scale.z == 6.0F &&
         migratedTransform.rotation.w == 1.0F);
  const std::vector<std::byte> migratedBaseline =
      ve::editor::encodeAuthoringDocument(migrated);
  const std::array<std::byte, 1> malformedPayload{std::byte{0}};
  assert(throws<std::runtime_error>([&] {
    ve::editor::decodeAuthoringDocument(
        migrated,
        oneComponentDocument(ve::kTransformSceneType, 1U, malformedPayload));
  }));
  assert(ve::editor::encodeAuthoringDocument(migrated) == migratedBaseline);
  assert(throws<std::runtime_error>([&] {
    ve::editor::decodeAuthoringDocument(
        migrated,
        oneComponentDocument(ve::kTransformSceneType, 3U, identityTransform));
  }));
  assert(ve::editor::encodeAuthoringDocument(migrated) == migratedBaseline);

  ve::editor::AuthoringDocument document;
  const ve::SceneEntityId root = document.create("Root");
  const ve::SceneEntityId child = document.create("Child", root);
  const ve::SceneEntityId sibling = document.create("Sibling");
  assert(document.dirty());
  document.markSaved();
  assert(!document.dirty());
  document.reparent(sibling, root);
  assert(document.find(sibling)->parent == root);
  document.undo();
  assert(!document.find(sibling)->parent.valid());
  document.redo();
  assert(document.find(sibling)->parent == root);
  const std::vector<std::byte> beforeCycle =
      ve::editor::encodeAuthoringDocument(document);
  assert(throws<std::runtime_error>([&] { document.reparent(root, child); }));
  assert(ve::editor::encodeAuthoringDocument(document) == beforeCycle);
  assert(throws<std::invalid_argument>(
      [&] { document.reparent(root, {99U, 99U}); }));
  assert(ve::editor::encodeAuthoringDocument(document) == beforeCycle);

  document.select(child);
  document.select(sibling, true);
  const std::array selected{child, sibling};
  document.setProperty(selected, ve::kTransformSceneType,
                       ve::stableSceneId("ve.scene.transform.translation"),
                       ve::Vec3{2.0F, 3.0F, 4.0F}, "Move selection");
  for (const ve::SceneEntityId id : selected) {
    const ve::TransformTRS transform = ve::decodeAuthoringTransform(
        document.component(id, ve::kTransformSceneType)->payload);
    assert(transform.translation.x == 2.0F && transform.translation.z == 4.0F);
  }
  document.undo();
  for (const ve::SceneEntityId id : selected) {
    assert(ve::decodeAuthoringTransform(
               document.component(id, ve::kTransformSceneType)->payload)
               .translation.x == 0.0F);
  }
  document.redo();

  ve::editor::AuthoringComponent unknown{
      ve::stableSceneId("vendor.future.component"),
      7U,
      {std::byte{1}, std::byte{2}, std::byte{3}}};
  document.setComponent(child, unknown);
  const std::vector<std::byte> withUnknown =
      ve::editor::encodeAuthoringDocument(document);
  ve::editor::AuthoringDocument reopened;
  ve::editor::decodeAuthoringDocument(reopened, withUnknown);
  assert(reopened.component(child, unknown.type) != nullptr);
  assert(reopened.component(child, unknown.type)->payload == unknown.payload);
  assert(ve::editor::encodeAuthoringDocument(reopened) == withUnknown);
  assert(throws<std::runtime_error>(
      [&] { static_cast<void>(ve::editor::cookAuthoringDocument(reopened)); }));

  ve::editor::AuthoringDocument divergence;
  const ve::SceneEntityId divergenceId = divergence.create("A");
  divergence.rename(divergenceId, "B");
  const ve::editor::AuthoringEntity beforePreview =
      *divergence.find(divergenceId);
  ve::editor::AuthoringEntity afterPreview = beforePreview;
  afterPreview.name = "C";
  divergence.applyPreview(
      {"Preview", {{divergenceId, beforePreview, afterPreview}}});
  const std::vector<std::byte> previewState =
      ve::editor::encodeAuthoringDocument(divergence);
  assert(throws<std::runtime_error>([&] { divergence.undo(); }));
  assert(ve::editor::encodeAuthoringDocument(divergence) == previewState);
  assert(divergence.canUndo());

  ve::editor::AuthoringLimits boundedHistoryLimits;
  boundedHistoryLimits.maximumHistory = 2U;
  ve::editor::AuthoringDocument boundedHistory{ve::builtinSceneTypeRegistry(),
                                               boundedHistoryLimits};
  const ve::SceneEntityId boundedId = boundedHistory.create("A");
  boundedHistory.rename(boundedId, "B");
  boundedHistory.rename(boundedId, "C");
  boundedHistory.undo();
  assert(boundedHistory.find(boundedId)->name == "B");
  boundedHistory.undo();
  assert(boundedHistory.find(boundedId)->name == "A");
  assert(throws<std::logic_error>([&] { boundedHistory.undo(); }));

  ve::editor::AuthoringDocument deletion;
  const ve::SceneEntityId deleteRoot = deletion.create("DeleteRoot");
  const ve::SceneEntityId deleteChild =
      deletion.create("DeleteChild", deleteRoot);
  deletion.erase(deleteRoot);
  assert(deletion.entities().empty());
  deletion.undo();
  assert(deletion.find(deleteRoot) != nullptr &&
         deletion.find(deleteChild) != nullptr);

  const std::filesystem::path temporary =
      std::filesystem::temp_directory_path() /
      ("volkengine-authoring-" +
       std::to_string(reinterpret_cast<std::uintptr_t>(&document)));
  std::filesystem::create_directories(temporary);
  const std::filesystem::path assetRoot{VOLKENGINE_TEST_ASSET_DIR};
  const ve::ImportedGltfScene reference =
      ve::importGltfScene(assetRoot / "reference_scene.gltf",
                          ve::builtin_assets::kReferenceSceneId);
  ve::editor::AuthoringDocument imported =
      ve::editor::importAuthoringScene(reference);
  assert(imported.entities().size() == reference.nodes.size());
  const ve::SceneEntityId added = imported.create("Created in editor");
  imported.reparent(added, imported.entities().front().id);
  imported.setProperty(std::span{&added, 1U}, ve::kTransformSceneType,
                       ve::stableSceneId("ve.scene.transform.translation"),
                       ve::Vec3{9.0F, 2.0F, -4.0F}, "Move created entity");
  imported.undo();
  ve::editor::EditorSession session;
  const ve::SceneEntityId pickedEntity =
      session.document().create("Pick target");
  session.document().setComponent(
      pickedEntity,
      {ve::kRenderableSceneType, 1U,
       ve::encodeAuthoringRenderable({{11U, 12U}, {13U, 14U}, true})});
  ve::Camera pickingCamera;
  session.document().setProperty(
      std::span{&pickedEntity, 1U}, ve::kTransformSceneType,
      ve::stableSceneId("ve.scene.transform.translation"),
      ve::Vec3{0.0F, 0.55F, 0.0F}, "Position pick target");
  assert(session.pickAndSelect(pickingCamera, 640.0F, 360.0F, 1280.0F, 720.0F,
                               false));
  assert(session.document().selection().size() == 1U &&
         session.document().selection()[0] == pickedEntity);
  assert(throws<std::invalid_argument>([&] {
    static_cast<void>(session.pick(pickingCamera, 0.0F, 0.0F, 0.0F, 720.0F));
  }));
  const ve::SceneEntityId secondGestureEntity =
      session.document().create("Second gesture target");
  session.document().select(pickedEntity);
  session.document().select(secondGestureEntity, true);
  session.beginTranslateGesture();
  session.previewTranslation({0.26F, 0.0F, 0.0F}, 0.5F);
  session.previewTranslation({0.76F, 0.0F, 0.0F}, 0.5F);
  session.commitTranslateGesture();
  for (const ve::SceneEntityId id :
       std::array{pickedEntity, secondGestureEntity}) {
    assert(
        ve::decodeAuthoringTransform(
            session.document().component(id, ve::kTransformSceneType)->payload)
            .translation.x == 1.0F);
  }
  session.document().undo();
  for (const ve::SceneEntityId id :
       std::array{pickedEntity, secondGestureEntity}) {
    assert(
        ve::decodeAuthoringTransform(
            session.document().component(id, ve::kTransformSceneType)->payload)
            .translation.x == 0.0F);
  }
  session.document().redo();
  session.beginTranslateGesture();
  session.previewTranslation({2.0F, 0.0F, 0.0F}, 0.0F);
  session.cancelTranslateGesture();
  assert(ve::decodeAuthoringTransform(
             session.document()
                 .component(pickedEntity, ve::kTransformSceneType)
                 ->payload)
             .translation.x == 1.0F);

  imported.redo();
  const std::filesystem::path authoringPath = temporary / "roundtrip.veauthor";
  ve::editor::saveAuthoringDocument(imported, authoringPath);
  assert(!imported.dirty());
  ve::editor::AuthoringDocument loadedAuthoring;
  ve::editor::loadAuthoringDocument(loadedAuthoring, authoringPath);
  assert(ve::editor::encodeAuthoringDocument(loadedAuthoring) ==
         ve::editor::encodeAuthoringDocument(imported));

  const ve::CookedWorld cooked =
      ve::editor::cookAuthoringDocument(loadedAuthoring);
  const std::vector<std::byte> cookedBytes = ve::encodeCookedWorld(cooked);
  const ve::CookedWorld decodedCooked = ve::decodeCookedWorld(cookedBytes);
  assert(ve::encodeCookedWorld(decodedCooked) == cookedBytes);
  const std::filesystem::path cookedPath = temporary / "roundtrip.vecooked";
  ve::saveCookedWorld(cooked, cookedPath);
  assert(ve::encodeCookedWorld(ve::loadCookedWorld(cookedPath)) == cookedBytes);

  const ve::CookedWorldAssetResolver resolver{nullptr, resolveMesh,
                                              resolveMaterial, resolveBounds};
  ve::World runtime;
  ve::instantiateCookedWorld(runtime, decodedCooked, resolver);
  assert(runtime.entityCount() == loadedAuthoring.entities().size());
  assert(runtime.componentCount<ve::WorldSceneIdentity>() ==
         runtime.entityCount());
  assert(runtime.componentCount<ve::WorldSceneTransform>() ==
         runtime.entityCount());
  assert(runtime.componentCount<ve::WorldSceneRenderable>() ==
         reference.meshes.size());
  ve::World retained;
  const ve::World::Entity retainedEntity = retained.createEntity();
  retained.emplace<ve::WorldSceneIdentity>(
      retainedEntity, ve::WorldSceneIdentity{{1U, 1U}, "Retained"});
  const ve::CookedWorldAssetResolver invalidResolver{
      nullptr, [](void *, ve::AssetId) { return ve::MeshAssetHandle{}; },
      resolveMaterial, resolveBounds};
  assert(throws<std::runtime_error>([&] {
    ve::instantiateCookedWorld(retained, decodedCooked, invalidResolver);
  }));
  assert(retained.entityCount() == 1U &&
         retained.componentCount<ve::WorldSceneIdentity>() == 1U);

  std::error_code error;
  std::filesystem::remove_all(temporary, error);
  return 0;
}
