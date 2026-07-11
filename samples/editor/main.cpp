#include "assets/GltfImporter.hpp"
#include "core/Application.hpp"
#include "core/WorldScheduler.hpp"
#include "editor/EditorShell.hpp"
#include "scene/CookedWorld.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

struct EditorRuntime {
  ve::Application *application = nullptr;
  ve::World *world = nullptr;
  bool reloadPending = false;
  std::filesystem::path pendingCookedPath;
};

struct OverlayContext {
  ve::editor::EditorShell *shell = nullptr;
  ve::Application *application = nullptr;
};

void drawEditor(void *context, const ve::RendererOverlayFrame &frame) {
  auto &overlay = *static_cast<OverlayContext *>(context);
  overlay.shell->draw(frame, overlay.application->jobStats());
}

void requestRuntimeReload(void *context, const std::filesystem::path &path) {
  auto &runtime = *static_cast<EditorRuntime *>(context);
  runtime.pendingCookedPath = path;
  runtime.reloadPending = true;
}

void publishCookedWorld(void *context, ve::World &,
                        ve::WorldSystemScheduler::CommandWriter &,
                        const ve::InputState &, double, double) {
  auto &runtime = *static_cast<EditorRuntime *>(context);
  if (!runtime.reloadPending)
    return;
  const ve::CookedWorld candidate =
      ve::loadCookedWorld(runtime.pendingCookedPath);
  runtime.application->instantiateCookedWorld(*runtime.world, candidate);
  runtime.reloadPending = false;
}

std::uint64_t parseUnsigned(const std::string_view value,
                            const std::string_view option) {
  std::uint64_t result = 0U;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::invalid_argument(std::string(option) +
                                " requires an unsigned integer");
  }
  return result;
}

} // namespace

int main(int argc, char **argv) {
  try {
    ve::EngineConfig config;
    config.applicationName = "VolkEngine Editor";
    config.debugOverlay = true;
    ve::RunOptions run;
    run.maxFrames = 0U;
    bool smoke = false;
    for (int index = 1; index < argc; ++index) {
      const std::string_view option{argv[index]};
      const auto value = [&]() -> std::string_view {
        if (++index >= argc)
          throw std::invalid_argument(std::string(option) +
                                      " requires a value");
        return argv[index];
      };
      if (option == "--frames") {
        run.maxFrames = parseUnsigned(value(), option);
      } else if (option == "--screenshot") {
        run.screenshotPath = value();
      } else if (option == "--screenshot-frame") {
        run.screenshotFrame = parseUnsigned(value(), option);
      } else if (option == "--run-summary") {
        run.summaryPath = value();
      } else if (option == "--no-vsync") {
        config.vsync = false;
      } else if (option == "--validation") {
        config.validation = true;
      } else if (option == "--require-validation") {
        config.validation = true;
        config.requireValidation = true;
      } else if (option == "--editor-smoke") {
        smoke = true;
      } else if (option == "--help") {
        std::cout << "Usage: VolkEngineEditor [--frames N] [--editor-smoke] "
                     "[--no-vsync] [--validation|--require-validation] "
                     "[--screenshot FILE.ppm] [--screenshot-frame N] "
                     "[--run-summary FILE.json]\n";
        return 0;
      } else {
        throw std::invalid_argument("Unknown option " + std::string(option));
      }
    }

    ve::Application application{config};
    const ve::ImportedGltfScene reference =
        ve::importGltfScene(config.assetDirectory / "reference_scene.gltf",
                            ve::builtin_assets::kReferenceSceneId);
    ve::editor::EditorSession session;
    session.importScene(reference);
    const std::filesystem::path editorDirectory =
        config.cacheDirectory / "editor";
    std::filesystem::create_directories(editorDirectory);
    const std::filesystem::path authoringPath =
        editorDirectory / "reference.veauthor";
    const std::filesystem::path cookedPath =
        editorDirectory / "reference.vecooked";
    if (smoke) {
      const ve::SceneEntityId created =
          session.document().create("Editor Smoke Entity");
      session.document().reparent(created,
                                  session.document().entities().front().id);
      session.document().setProperty(
          std::span{&created, 1U}, ve::kTransformSceneType,
          ve::stableSceneId("ve.scene.transform.translation"),
          ve::Vec3{1.0F, 0.5F, -1.0F}, "Editor smoke transform");
      session.document().undo();
      session.document().redo();
    }
    session.save(authoringPath);
    session.cook(cookedPath);
    session.load(authoringPath);
    ve::World world;
    application.instantiateCookedWorld(world, ve::loadCookedWorld(cookedPath));

    EditorRuntime runtime{&application, &world, false, {}};
    ve::editor::EditorShell shell{
        session, reference, {&runtime, requestRuntimeReload}};
    OverlayContext overlay{&shell, &application};
    application.setRendererOverlay(drawEditor, &overlay);

    ve::WorldSystemScheduler scheduler;
    scheduler.addSystem({.name = "editor-runtime-publication",
                         .callback = publishCookedWorld,
                         .context = &runtime});
    scheduler.compile();
    return application.run(world, scheduler, run);
  } catch (const std::exception &error) {
    std::cerr << "Fatal editor error: " << error.what() << '\n';
    return 1;
  }
}
