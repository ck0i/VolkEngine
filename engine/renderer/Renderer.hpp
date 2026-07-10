#pragma once

#include "core/Camera.hpp"
#include <cstdint>
#include <string>

namespace ve {
struct SceneRenderList;


enum class RenderBackend {
    Vulkan
};

enum class TransferUploadSyncMode : std::uint8_t {
    SameQueueBarrier,
    QueueSemaphore
};

struct RenderDeviceInfo {
    RenderBackend backend = RenderBackend::Vulkan;
    std::string adapterName;
    std::uint32_t apiVersionMajor = 0;
    std::uint32_t apiVersionMinor = 0;
    std::uint32_t apiVersionPatch = 0;
    std::uint32_t maxImageDimension2D = 0;
    std::uint32_t maxDrawIndirectCount = 0;
    bool discreteGpu = false;
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool timestampQueries = false;
    bool validationEnabled = false;
    bool debugMarkers = false;
    bool memoryBudget = false;
    bool descriptorIndexing = false;
    bool bindlessSampledImagesSupported = false;
    bool multiDrawIndirect = false;
    bool drawIndirectFirstInstance = false;
    bool samplerAnisotropy = false;
    float maxSamplerAnisotropy = 1.0f;
    TransferUploadSyncMode transferUploadSync = TransferUploadSyncMode::SameQueueBarrier;
    bool indirectSceneDraws = false;
};
struct RenderStats {
    double cpuFrameMs = 0.0;
    double cpuSceneBuildMs = 0.0;
    double cpuPrepareMs = 0.0;
    double cpuCommandRecordMs = 0.0;
    double cpuQueueSubmitMs = 0.0;
    double frameDeltaMs = 0.0;
    double gpuFrameMs = 0.0;
    double gpuDepthPrepassMs = 0.0;
    double gpuHdrSceneMs = 0.0;
    double gpuFinalPassMs = 0.0;
    bool gpuTimestampsValid = false;
    double elapsedSeconds = 0.0;
    bool depthPrepassEnabled = false;
    unsigned scenePassCount = 0;
    unsigned sceneItemCount = 0;
    unsigned visibleItemCount = 0;
    unsigned sceneInstanceCapacity = 0;
    double sceneInstanceBufferMiB = 0.0;
    unsigned meshBatchCount = 0;
    unsigned drawCalls = 0;
    unsigned culledItemCount = 0;
    unsigned gridTileCount = 0;
    unsigned gridTilesCulled = 0;
    unsigned gridTilesAccepted = 0;
    unsigned gridTilesIntersected = 0;
    bool gridVisibilityCacheHit = false;
    unsigned sphereLodHighCount = 0;
    unsigned sphereLodMediumCount = 0;
    unsigned sphereLodLowCount = 0;
    unsigned gridVisibilityWorkItems = 0;
    bool indirectSceneDraws = false;
    std::uint64_t sceneTriangleCount = 0;
    std::uint64_t triangleCount = 0;
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void draw(const Camera& camera, const SceneRenderList& scene, double sceneBuildMs,
                      double elapsedSeconds, double frameDeltaMs) = 0;
    [[nodiscard]] virtual RenderStats stats() const = 0;
    [[nodiscard]] virtual const RenderDeviceInfo& deviceInfo() const = 0;
};

} // namespace ve
