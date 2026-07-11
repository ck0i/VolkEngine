#include "core/Application.hpp"
#include "core/RunSummary.hpp"
#include "core/WorldScheduler.hpp"

#include "core/Log.hpp"
#include "scene/CookedWorld.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace ve {
namespace {

const char* transferUploadSyncName(const TransferUploadSyncMode mode) noexcept {
    switch (mode) {
    case TransferUploadSyncMode::SameQueueBarrier:
        return "same-queue-barrier";
    case TransferUploadSyncMode::QueueSemaphore:
        return "queue-semaphore";
    }
    return "unknown";
}

RenderMaterial renderMaterial(const ImportedMaterial &material,
                              const VulkanRenderer &renderer) {
  RenderMaterial result{{material.baseColorFactor.x, material.baseColorFactor.y,
                         material.baseColorFactor.z, material.roughnessFactor},
                        {material.emissiveFactor.x, material.emissiveFactor.y,
                         material.emissiveFactor.z, material.metallicFactor},
                        {}};
  result.textures = renderer.materialTextureHandles(material.id);
  result.flags.y =
      static_cast<float>(material.alphaMode == MaterialAlphaMode::Mask
                             ? RenderMaterialClass::Masked
                             : RenderMaterialClass::Standard);
  if (material.alphaMode == MaterialAlphaMode::Mask) {
        result.flags.z = material.baseColorFactor.w > 0.0F
            ? material.alphaCutoff / material.baseColorFactor.w
            : 2.0F;
    }
    float normalScale = 1.0f;
    for (const ImportedTextureReference& texture : material.textures) {
        switch (texture.role) {
        case TextureRole::BaseColor:
        case TextureRole::MetallicRoughness:
        case TextureRole::Normal:
            if (texture.role == TextureRole::Normal) {
                normalScale = texture.scale;
      }
      break;
    case TextureRole::Occlusion:
    case TextureRole::Emissive:
      break;
    }
  }
  result.flags.w = normalScale;
  return result;
}

Mat4 nodeWorldTransform(const ImportedGltfScene &scene,
                        const std::size_t nodeIndex) {
  Mat4 model = scene.nodes[nodeIndex].localTransform;
  std::uint32_t parent = scene.nodes[nodeIndex].parent;
  for (std::size_t depth = 0;
       parent != std::numeric_limits<std::uint32_t>::max(); ++depth) {
    if (parent >= scene.nodes.size() || depth >= scene.nodes.size()) {
      throw std::runtime_error("Reference scene hierarchy is invalid");
    }
        model = scene.nodes[parent].localTransform * model;
        parent = scene.nodes[parent].parent;
    }
    return model;
}

Vec3 transformPoint(const Mat4 &matrix, const Vec3 point) noexcept {
  return {matrix.m[0] * point.x + matrix.m[4] * point.y +
              matrix.m[8] * point.z + matrix.m[12],
          matrix.m[1] * point.x + matrix.m[5] * point.y +
              matrix.m[9] * point.z + matrix.m[13],
          matrix.m[2] * point.x + matrix.m[6] * point.y +
              matrix.m[10] * point.z + matrix.m[14]};
}

float maximumScale(const Mat4 &matrix) noexcept {
    const Vec3 c0{matrix.m[0], matrix.m[1], matrix.m[2]};
    const Vec3 c1{matrix.m[4], matrix.m[5], matrix.m[6]};
    const Vec3 c2{matrix.m[8], matrix.m[9], matrix.m[10]};
    return std::sqrt(std::max({dot(c0, c0), dot(c1, c1), dot(c2, c2)}));
}

std::vector<SceneRenderItem> referenceDraws(const ImportedGltfScene& scene,
                                            const VulkanRenderer& renderer) {
    std::size_t drawCount = 0;
    for (const ImportedSceneNode& node : scene.nodes) {
        drawCount += node.meshPrimitives.size();
    }
    std::vector<SceneRenderItem> draws;
    draws.reserve(drawCount);
    for (std::size_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex) {
    const ImportedSceneNode &node = scene.nodes[nodeIndex];
    const Mat4 model = nodeWorldTransform(scene, nodeIndex);
    for (const AssetId meshId : node.meshPrimitives) {
      const auto mesh =
          std::ranges::find(scene.meshes, meshId, &ImportedMeshPrimitive::id);
      if (mesh == scene.meshes.end()) {
        throw std::runtime_error(
            "Reference node uses a missing mesh primitive");
      }
      const auto material = std::ranges::find(scene.materials, mesh->material,
                                              &ImportedMaterial::id);
            if (material == scene.materials.end()) {
                throw std::runtime_error("Reference mesh material is missing");
            }
            const MeshAssetHandle handle = referenceMeshHandle(scene, meshId);
            const MeshBounds bounds = renderer.meshBounds(handle);
            draws.push_back({transformPoint(model, bounds.center),
                             bounds.radius * maximumScale(model), handle, model,
                             renderMaterial(*material, renderer)});
        }
    }
  return draws;
}

} // namespace

Application::Application(EngineConfig config)
    : config_(std::move(config)),
      jobs_({config_.jobWorkerCount, config_.maximumJobs,
             config_.maximumJobDependencies, config_.jobTimelineCapacity}),
      referenceAssetCook_(jobs_, config_.assetDirectory,
                          config_.cacheDirectory / "assets", "vulkan-runtime"),
      simulationClock_(config_.fixedSimulationStepSeconds,
                       config_.maximumSimulationAccumulatedSeconds,
                       config_.maximumSimulationSubsteps),
      simulationInputTracker_{}, glfwRuntime_{}, window_(glfwRuntime_, config_),
      camera_{}, referenceAssets_(referenceAssetCook_.take()),
      renderer_(window_, config_, referenceAssets_), sceneRenderer_{},
      worldSceneExtractor_{}, clock_{} {
  const VkExtent2D extent = window_.framebufferExtent();
  if (extent.width > 0U && extent.height > 0U) {
    camera_.setAspect(static_cast<float>(extent.width) /
                      static_cast<float>(extent.height));
  }
  sceneRenderer_.setAuthoredSceneItems(
      referenceDraws(referenceAssets_.scene, renderer_));
}

void Application::setRendererOverlay(
    const RendererOverlayCallback callback, void *const context) noexcept {
  renderer_.setOverlayCallback(callback, context);
}

void Application::instantiateCookedWorld(
    World &destination, const CookedWorld &source) const {
  const CookedWorldAssetResolver resolver{
      const_cast<Application *>(this),
      [](void *context, const AssetId id) {
        const auto &application = *static_cast<const Application *>(context);
        return referenceMeshHandle(application.referenceAssets_.scene, id);
      },
      [](void *context, const AssetId id) {
        const auto &application = *static_cast<const Application *>(context);
        const auto found =
            std::ranges::find(application.referenceAssets_.scene.materials, id,
                              &ImportedMaterial::id);
        if (found == application.referenceAssets_.scene.materials.end()) {
          throw std::runtime_error(
              "Cooked world references an unknown material");
        }
        return renderMaterial(*found, application.renderer_);
      },
      [](void *context, const MeshAssetHandle mesh) {
        const auto &application = *static_cast<const Application *>(context);
        return application.renderer_.meshBounds(mesh);
      }};
  ve::instantiateCookedWorld(destination, source, resolver);
}

void Application::pollAssetReload(const double elapsedSeconds) {
  if (!config_.assetHotReload)
    return;
  if (pendingAssetReload_ == nullptr) {
    if (elapsedSeconds < nextAssetReloadSeconds_)
      return;
    pendingAssetReload_ = std::make_unique<ReferenceAssetCookTask>(
        jobs_, config_.assetDirectory, config_.cacheDirectory / "assets",
        "vulkan-runtime");
    return;
  }
  if (!pendingAssetReload_->finished())
    return;

  try {
    ReferenceAssetBundle candidate = pendingAssetReload_->take();
    if (candidate.database.serialize() !=
        referenceAssets_.database.serialize()) {
      renderer_.reloadReferenceAssets(std::move(candidate));
      sceneRenderer_.setAuthoredSceneItems(
          referenceDraws(referenceAssets_.scene, renderer_));
      logger()->info("Published asynchronous authored-asset reload");
    }
  } catch (const std::exception &error) {
    logger()->error(
        "Asynchronous authored-asset reload retained the active bundle: {}",
        error.what());
  }
  pendingAssetReload_.reset();
  nextAssetReloadSeconds_ = elapsedSeconds + 1.0;
}
int Application::run(const RunOptions &options) {
  return runInternal(nullptr, nullptr, nullptr, nullptr, options);
}

int Application::run(World& world, const RunOptions& options) {
  return runInternal(&world, nullptr, nullptr, nullptr, options);
}

int Application::run(World &world, const WorldUpdateCallback update,
                     const RunOptions &options) {
  return runInternal(&world, update, nullptr, nullptr, options);
}

int Application::runWithInput(World &world,
                              const WorldInputUpdateCallback update,
                              const RunOptions &options) {
  return runInternal(&world, nullptr, update, nullptr, options);
}

int Application::run(World &world, WorldSystemScheduler &scheduler,
                     const RunOptions &options) {
  if (!scheduler.compiled()) {
    throw std::logic_error(
        "Application requires a compiled World system scheduler");
  }
  return runInternal(&world, nullptr, nullptr, &scheduler, options);
}

int Application::runInternal(World *world, const WorldUpdateCallback update,
                             const WorldInputUpdateCallback inputUpdate,
                             WorldSystemScheduler *const scheduler,
                             const RunOptions &options) {
    logger()->info("Entering main loop");
    if (options.acquireRecoverySmoke) {
        renderer_.armAcquireRecoverySmoke();
    }
    double titleUpdateSeconds = 0.0;
    std::uint64_t titleUpdateFrames = 0;
    bool screenshotRequested = false;
    std::uint64_t renderedFrames = 0;
    BoundedMetricSamples cpuFrameSamples;
    BoundedMetricSamples cpuSceneBuildSamples;
    BoundedMetricSamples cpuCommandRecordSamples;
    BoundedMetricSamples cpuQueueSubmitSamples;
    BoundedMetricSamples gpuFrameSamples;
    while (!window_.shouldClose()) {
        const FrameTiming timing = clock_.tick();
        window_.pollEvents();

        if (options.resizeSmoke && timing.frameIndex == 45U) {
            window_.setSize(1024, 640);
        }
        if (options.resizeSmoke && timing.frameIndex == 90U) {
            window_.setSize(config_.initialWidth, config_.initialHeight);
    }

    const InputState input = window_.pollInput();
    const double cameraDeltaSeconds =
        clampDeltaSeconds(timing.deltaSeconds, 0.05);
    window_.updateCamera(camera_, input,
                         static_cast<float>(cameraDeltaSeconds));
    simulationInputTracker_.accumulate(input);
    const FixedStepBatch simulationBatch =
        simulationClock_.advance(timing.deltaSeconds);
    for (std::uint32_t stepIndex = 0; stepIndex < simulationBatch.stepCount;
         ++stepIndex) {
      const InputState stepInput = simulationInputTracker_.consume();
      const double stepElapsedSeconds =
          simulationBatch.elapsedSecondsForStep(stepIndex);
      if (world != nullptr && (scheduler != nullptr || inputUpdate != nullptr ||
                               update != nullptr)) {
        worldSceneExtractor_.prepareSimulationStep(*world);
        try {
          if (scheduler != nullptr) {
            (void)scheduler->execute(jobs_, *world, stepInput,
                                     stepElapsedSeconds,
                                     simulationBatch.stepSeconds);
          } else if (inputUpdate != nullptr) {
            inputUpdate(*world, stepInput, stepElapsedSeconds,
                        simulationBatch.stepSeconds);
          } else {
            update(*world, stepElapsedSeconds, simulationBatch.stepSeconds);
          }
                } catch (...) {
                    worldSceneExtractor_.invalidateSimulationState();
                    throw;
                }
                worldSceneExtractor_.captureSimulationStep(*world);
      }
    }

    pollAssetReload(timing.elapsedSeconds);

    const VkExtent2D extent = window_.framebufferExtent();
    if (extent.width > 0U && extent.height > 0U) {
      camera_.setAspect(static_cast<float>(extent.width) /
                              static_cast<float>(extent.height));
        }

        if (!screenshotRequested && !options.screenshotPath.empty() &&
            renderedFrames == options.screenshotFrame) {
            renderer_.requestScreenshot(options.screenshotPath);
            screenshotRequested = true;
        }

        const auto sceneBuildStart = std::chrono::steady_clock::now();
        const double authoredSceneSeconds =
            options.scenarioName == "interactive"
                ? simulationClock_.elapsedSeconds()
            : static_cast<double>(renderedFrames) / 60.0;
    const SceneRenderList &renderItems =
        world != nullptr
            ? worldSceneExtractor_.build(*world,
                                         simulationBatch.interpolationAlpha)
            : sceneRenderer_.build(
                  authoredSceneSeconds, config_.materialGridRows,
                  config_.materialGridColumns, config_.materialGridTileRows,
                  config_.materialGridTileColumns);
    const double sceneBuildMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sceneBuildStart)
            .count();
    renderer_.draw(camera_, renderItems, sceneBuildMs, timing.elapsedSeconds,
                   timing.deltaSeconds * 1000.0);
    if (renderedFrames >= options.warmupFrames) {
      const RenderStats sample = renderer_.stats();
      cpuFrameSamples.add(sample.cpuFrameMs);
      cpuSceneBuildSamples.add(sample.cpuSceneBuildMs);
      cpuCommandRecordSamples.add(sample.cpuCommandRecordMs);
      cpuQueueSubmitSamples.add(sample.cpuQueueSubmitMs);
      if (sample.gpuTimestampsValid)
        gpuFrameSamples.add(sample.gpuFrameMs);
    }
    ++renderedFrames;
    titleUpdateSeconds += timing.deltaSeconds;
    ++titleUpdateFrames;
    if (titleUpdateSeconds >= 0.5) {
      const RenderStats stats = renderer_.stats();
      const double fps =
          static_cast<double>(titleUpdateFrames) / titleUpdateSeconds;
      std::array<char, 256> title{};
      std::array<char, 32> gpuTitle{};
      if (stats.gpuTimestampsValid) {
        std::snprintf(gpuTitle.data(), gpuTitle.size(), "%.2f ms",
                      stats.gpuFrameMs);
      } else {
        std::snprintf(gpuTitle.data(), gpuTitle.size(), "pending");
      }
      const JobSystemStats jobStats = jobs_.stats();
      std::snprintf(title.data(), title.size(),
                    "%s | %.0f FPS | Frame %.2f ms | CPU %.2f ms | GPU %s | "
                    "Draws %u | Batches %u | Passes %u | Culled items %u | "
                    "Jobs %u active / %u running",
                    config_.applicationName.c_str(), fps, stats.frameDeltaMs,
                    stats.cpuFrameMs, gpuTitle.data(), stats.drawCalls,
                    stats.meshBatchCount, stats.scenePassCount,
                    stats.culledItemCount, jobStats.activeJobs,
                    jobStats.runningJobs);
      window_.setTitle(title.data());
      titleUpdateSeconds = 0.0;
      titleUpdateFrames = 0;
        }

        if (options.maxFrames > 0 && timing.frameIndex + 1U >= options.maxFrames) {
            window_.requestClose();
        }
  }

  renderer_.waitIdle();
  logger()->info("Simulation advanced {:.3f} s at {:.3f} ms fixed steps "
                 "({:.3f} ms retained)",
                 simulationClock_.elapsedSeconds(),
                 simulationClock_.stepSeconds() * 1000.0,
                 simulationClock_.retainedSeconds() * 1000.0);
    const RenderStats finalStats = renderer_.stats();
  const RenderDeviceInfo &finalDevice = renderer_.deviceInfo();
  std::array<char, 192> finalGpu{};
  if (finalStats.gpuTimestampsValid) {
    std::snprintf(finalGpu.data(), finalGpu.size(),
                  "%.3f ms (lights %.3f / cull %.3f / shadows %.3f / depth "
                  "%.3f / HDR %.3f / Hi-Z %.3f / final %.3f)",
                  finalStats.gpuFrameMs, finalStats.gpuLightAssignmentMs,
                  finalStats.gpuCullMs, finalStats.gpuShadowMs,
                  finalStats.gpuDepthPrepassMs, finalStats.gpuHdrSceneMs,
            finalStats.gpuDepthPyramidMs, finalStats.gpuFinalPassMs);
  } else {
    std::snprintf(finalGpu.data(), finalGpu.size(), "pending/unavailable");
  }
  logger()->info(
      "Exited cleanly. Last frame: frame {:.3f} ms, CPU {:.3f} ms (scene "
      "{:.3f} / prepare {:.3f} / record {:.3f} / submit {:.3f}), GPU {}, "
      "prepass {}, scene passes {}, batches {}, submission {}, upload sync {}, "
      "visible {}/{}, draws {}, culled items {}, triangles scene/submitted "
      "{}/{}, grid tiles {} (accepted {}, culled {}, intersected {}), grid "
      "cache {} (work {}), instance cap {} ({:.2f} MiB), sphere LOD instances "
      "{}/{}/{}, lights {} (overflow {}), shadows {}/{} (overflow {}), probes "
      "{}, exposure {:.2f}",
      finalStats.frameDeltaMs, finalStats.cpuFrameMs,
      finalStats.cpuSceneBuildMs, finalStats.cpuPrepareMs,
      finalStats.cpuCommandRecordMs, finalStats.cpuQueueSubmitMs,
      finalGpu.data(), finalStats.depthPrepassEnabled ? "on" : "off",
      finalStats.scenePassCount, finalStats.meshBatchCount,
      finalStats.indirectSceneDraws ? "multi-draw-indirect" : "direct",
      transferUploadSyncName(finalDevice.transferUploadSync),
      finalStats.visibleItemCount, finalStats.sceneItemCount,
      finalStats.drawCalls, finalStats.culledItemCount,
      finalStats.sceneTriangleCount, finalStats.triangleCount,
      finalStats.gridTileCount, finalStats.gridTilesAccepted,
      finalStats.gridTilesCulled, finalStats.gridTilesIntersected,
      finalStats.gridVisibilityCacheHit ? "hit" : "miss",
      finalStats.gridVisibilityWorkItems, finalStats.sceneInstanceCapacity,
      finalStats.sceneInstanceBufferMiB, finalStats.sphereLodHighCount,
      finalStats.sphereLodMediumCount, finalStats.sphereLodLowCount,
      finalStats.localLightCount, finalStats.lightListOverflowCount,
      finalStats.shadowViewCount, finalStats.shadowAtlasCapacity,
      finalStats.shadowAtlasOverflowCount, finalStats.reflectionProbeCount,
      finalStats.effectiveExposure);
  const RunMetricDistributions distributions{
      cpuFrameSamples.distribution(), cpuSceneBuildSamples.distribution(),
      cpuCommandRecordSamples.distribution(),
      cpuQueueSubmitSamples.distribution(), gpuFrameSamples.distribution()};
  const JobSystemStats jobStats = jobs_.stats();
  logger()->info(
      "Job system: {} workers, {} submitted, {} succeeded, {} failed, {} "
      "cancelled, {} steals, {:.3f} ms worker time, queue high-water {}",
      jobStats.workerCount, jobStats.submitted, jobStats.succeeded,
      jobStats.failed, jobStats.cancelled, jobStats.steals,
      static_cast<double>(jobStats.executedNanoseconds) / 1.0e6,
      jobStats.queueHighWatermark);
  if (!options.summaryPath.empty()) {
    writeRunSummaryAtomic(options.summaryPath,
                          RunSummary{config_, options, finalDevice, finalStats,
                                     distributions, jobStats, renderedFrames,
                                     0});
    logger()->info("Saved run summary {}", options.summaryPath.string());
  }
  return 0;
}

} // namespace ve
