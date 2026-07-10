#include "core/Application.hpp"

#include "core/Log.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <spdlog/spdlog.h>

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


} // namespace


Application::Application(EngineConfig config)
    : config_(std::move(config)), window_(config_), camera_{}, renderer_(window_, config_), sceneRenderer_{}, clock_{} {
    const VkExtent2D extent = window_.framebufferExtent();
    if (extent.width > 0U && extent.height > 0U) {
        camera_.setAspect(static_cast<float>(extent.width) / static_cast<float>(extent.height));
    }
    sceneRenderer_.setImportedModelBounds(renderer_.meshBounds(SceneMeshId::ImportedModel));
}
int Application::run(const RunOptions& options) {
    logger()->info("Entering main loop");
    double titleUpdateSeconds = 0.0;
    std::uint64_t titleUpdateFrames = 0;
    bool screenshotRequested = false;
    while (!window_.shouldClose()) {
        const FrameTiming timing = clock_.tick();
        window_.pollEvents();

        if (options.resizeSmoke && timing.frameIndex == 45U) {
            window_.setSize(1024, 640);
        }
        if (options.resizeSmoke && timing.frameIndex == 90U) {
            window_.setSize(config_.initialWidth, config_.initialHeight);
        }

        const double previousSimulationElapsedSeconds = simulationElapsedSeconds_;
        simulationElapsedSeconds_ = advanceSimulationSeconds(simulationElapsedSeconds_, timing.deltaSeconds, 0.05);
        const float simulationDelta = static_cast<float>(simulationElapsedSeconds_ - previousSimulationElapsedSeconds);
        window_.updateCamera(camera_, simulationDelta);
        const VkExtent2D extent = window_.framebufferExtent();
        if (extent.width > 0U && extent.height > 0U) {
            camera_.setAspect(static_cast<float>(extent.width) / static_cast<float>(extent.height));
        }

        if (!screenshotRequested && !options.screenshotPath.empty()) {
            renderer_.requestScreenshot(options.screenshotPath);
            screenshotRequested = true;
        }

        const auto sceneBuildStart = std::chrono::steady_clock::now();
        const SceneRenderList& renderItems = sceneRenderer_.build(
            simulationElapsedSeconds_,
            config_.materialGridRows,
            config_.materialGridColumns,
            config_.materialGridTileRows,
            config_.materialGridTileColumns);
        const double sceneBuildMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sceneBuildStart).count();
        renderer_.draw(camera_, renderItems, sceneBuildMs, timing.elapsedSeconds, timing.deltaSeconds * 1000.0);
        titleUpdateSeconds += timing.deltaSeconds;
        ++titleUpdateFrames;
        if (titleUpdateSeconds >= 0.5) {
            const RenderStats stats = renderer_.stats();
            const double fps = static_cast<double>(titleUpdateFrames) / titleUpdateSeconds;
            std::array<char, 256> title{};
            std::array<char, 32> gpuTitle{};
            if (stats.gpuTimestampsValid) {
                std::snprintf(gpuTitle.data(), gpuTitle.size(), "%.2f ms", stats.gpuFrameMs);
            } else {
                std::snprintf(gpuTitle.data(), gpuTitle.size(), "pending");
            }
            std::snprintf(title.data(), title.size(), "%s | %.0f FPS | Frame %.2f ms | CPU %.2f ms | GPU %s | Draws %u | Batches %u | Passes %u | Culled items %u",
                          config_.applicationName.c_str(), fps, stats.frameDeltaMs, stats.cpuFrameMs, gpuTitle.data(),
                          stats.drawCalls, stats.meshBatchCount, stats.scenePassCount, stats.culledItemCount);
            window_.setTitle(title.data());
            titleUpdateSeconds = 0.0;
            titleUpdateFrames = 0;
        }

        if (options.maxFrames > 0 && timing.frameIndex + 1U >= options.maxFrames) {
            window_.requestClose();
        }
    }

    renderer_.waitIdle();
    const RenderStats finalStats = renderer_.stats();
    const RenderDeviceInfo& finalDevice = renderer_.deviceInfo();
    std::array<char, 128> finalGpu{};
    if (finalStats.gpuTimestampsValid) {
        std::snprintf(finalGpu.data(), finalGpu.size(), "%.3f ms (depth %.3f / HDR %.3f / final %.3f)",
                      finalStats.gpuFrameMs, finalStats.gpuDepthPrepassMs, finalStats.gpuHdrSceneMs, finalStats.gpuFinalPassMs);
    } else {
        std::snprintf(finalGpu.data(), finalGpu.size(), "pending/unavailable");
    }
    logger()->info("Exited cleanly. Last frame: frame {:.3f} ms, CPU {:.3f} ms (scene {:.3f} / prepare {:.3f} / record {:.3f} / submit {:.3f}), GPU {}, prepass {}, scene passes {}, batches {}, submission {}, upload sync {}, visible {}/{}, draws {}, culled items {}, triangles scene/submitted {}/{}, grid tiles {} (accepted {}, culled {}, intersected {}), grid cache {} (work {}), instance cap {} ({:.2f} MiB), sphere LOD instances {}/{}/{}",
                   finalStats.frameDeltaMs, finalStats.cpuFrameMs,
                   finalStats.cpuSceneBuildMs, finalStats.cpuPrepareMs, finalStats.cpuCommandRecordMs, finalStats.cpuQueueSubmitMs,
                   finalGpu.data(),
                   finalStats.depthPrepassEnabled ? "on" : "off", finalStats.scenePassCount, finalStats.meshBatchCount,
                   finalStats.indirectSceneDraws ? "multi-draw-indirect" : "direct",
                   transferUploadSyncName(finalDevice.transferUploadSync), finalStats.visibleItemCount,
                   finalStats.sceneItemCount, finalStats.drawCalls, finalStats.culledItemCount, finalStats.sceneTriangleCount, finalStats.triangleCount,
                   finalStats.gridTileCount, finalStats.gridTilesAccepted, finalStats.gridTilesCulled, finalStats.gridTilesIntersected,
                   finalStats.gridVisibilityCacheHit ? "hit" : "miss", finalStats.gridVisibilityWorkItems,
                   finalStats.sceneInstanceCapacity, finalStats.sceneInstanceBufferMiB,
                   finalStats.sphereLodHighCount, finalStats.sphereLodMediumCount, finalStats.sphereLodLowCount);
    return 0;
}

} // namespace ve
