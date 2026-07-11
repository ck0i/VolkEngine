#pragma once

#include "assets/ReferenceAssetPipeline.hpp"
#include "core/Camera.hpp"
#include "core/Config.hpp"
#include "core/JobSystem.hpp"
#include "core/RunSummary.hpp"
#include "core/Time.hpp"
#include "core/World.hpp"
#include "platform/Window.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/vulkan/VulkanRenderer.hpp"
#include <memory>

namespace ve {

class WorldSystemScheduler;
struct CookedWorld;

class Application {
public:
  using ReferenceAssetAugmentCallback = void (*)(void *context,
                                                 ReferenceAssetBundle &bundle);
  explicit Application(EngineConfig config);
  Application(EngineConfig config, ReferenceAssetAugmentCallback augment,
              void *context);
  using WorldUpdateCallback = void (*)(World &, double simulationElapsedSeconds,
                                       double simulationDeltaSeconds);
  using WorldInputUpdateCallback = void (*)(World &, const InputState &,
                                            double simulationElapsedSeconds,
                                            double simulationDeltaSeconds);
  using FrameUpdateCallback = bool (*)(void *context,
                                       Application &application, World &world,
                                       Camera &camera, std::uint64_t frameIndex);
  int run(const RunOptions &options);
  int run(World &world, const RunOptions &options);
  int run(World &world, WorldUpdateCallback update, const RunOptions &options);
  int runWithInput(World &world, WorldInputUpdateCallback update,
                   const RunOptions &options);
  int run(World &world, WorldSystemScheduler &scheduler,
          const RunOptions &options);
  void setRendererOverlay(RendererOverlayCallback callback,
                          void *context) noexcept;
  [[nodiscard]] JobSystemStats jobStats() const { return jobs_.stats(); }
  [[nodiscard]] RenderStats renderStats() const { return renderer_.stats(); }
  void instantiateCookedWorld(World &destination,
                              const CookedWorld &source) const;
  [[nodiscard]] JobSystem &jobSystem() noexcept { return jobs_; }
  void setFrameUpdateCallback(FrameUpdateCallback callback,
                              void *context) noexcept;
  void configureStreamingRun(std::string manifestHash,
                             std::string contentHash,
                             std::uint64_t budgetBytes);
  void updateStreamingRunStats(const StreamingRunStats &stats,
                               StreamingFrameSample sample);
  void configureLandscapeRun(LandscapeRunStats stats);
  void updateLandscapeRunVisibility();

private:
  int runInternal(World *world, WorldUpdateCallback update,
                  WorldInputUpdateCallback inputUpdate,
                  WorldSystemScheduler *scheduler, const RunOptions &options);
  void pollAssetReload(double elapsedSeconds);
  ReferenceAssetAugmentCallback referenceAssetAugment_ = nullptr;
  void *referenceAssetAugmentContext_ = nullptr;
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
  FrameUpdateCallback frameUpdateCallback_ = nullptr;
  LandscapeRunStats landscapeStats_;
  void *frameUpdateContext_ = nullptr;
  StreamingRunStats streamingStats_;
    Clock clock_;
};

} // namespace ve
