#pragma once

#include "assets/ReferenceAssetPipeline.hpp"
#include "core/Camera.hpp"
#include "core/Config.hpp"
#include "core/JobSystem.hpp"
#include "core/Time.hpp"
#include "core/World.hpp"
#include "platform/Window.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/vulkan/VulkanRenderer.hpp"
#include <memory>

namespace ve {

class WorldSystemScheduler;

class Application {
public:
  explicit Application(EngineConfig config);
  using WorldUpdateCallback = void (*)(World &, double simulationElapsedSeconds,
                                       double simulationDeltaSeconds);
  using WorldInputUpdateCallback = void (*)(World &, const InputState &,
                                            double simulationElapsedSeconds,
                                            double simulationDeltaSeconds);
  int run(const RunOptions &options);
  int run(World &world, const RunOptions &options);
  int run(World &world, WorldUpdateCallback update, const RunOptions &options);
  int runWithInput(World &world, WorldInputUpdateCallback update,
                   const RunOptions &options);
  int run(World &world, WorldSystemScheduler &scheduler,
          const RunOptions &options);

private:
  int runInternal(World *world, WorldUpdateCallback update,
                  WorldInputUpdateCallback inputUpdate,
                  WorldSystemScheduler *scheduler, const RunOptions &options);
  void pollAssetReload(double elapsedSeconds);
  EngineConfig config_;
  JobSystem jobs_;
  ReferenceAssetCookTask referenceAssetCook_;
  FixedStepClock simulationClock_;
  InputTracker simulationInputTracker_;
  GlfwRuntime glfwRuntime_;
  Window window_;
  Camera camera_;
  ReferenceAssetBundle referenceAssets_;
  std::unique_ptr<ReferenceAssetCookTask> pendingAssetReload_;
  double nextAssetReloadSeconds_ = 0.0;
  VulkanRenderer renderer_;
  DemoSceneRenderer sceneRenderer_;
  WorldSceneExtractor worldSceneExtractor_;
    Clock clock_;
};

} // namespace ve
