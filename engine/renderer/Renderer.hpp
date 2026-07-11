#pragma once

#include "core/Camera.hpp"
#include <array>
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
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
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
    bool samplerFilterMinmax = false;
    bool computeSubgroupBallot = false;
    bool shaderDemoteToHelperInvocation = false;
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
    double gpuLightAssignmentMs = 0.0;
    double gpuCullMs = 0.0;
    double gpuShadowMs = 0.0;
    double gpuDepthPrepassMs = 0.0;
    double gpuHdrSceneMs = 0.0;
    double gpuDepthPyramidMs = 0.0;
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
    unsigned sceneClusterCount = 0;
    unsigned visibleCullingUnitCount = 0;
    unsigned testedCullingUnitCount = 0;
    unsigned occludedCullingUnitCount = 0;
    bool cullingUnitsAreClusters = false;
    unsigned materialDescriptorCount = 0;
    unsigned materialDescriptorCapacity = 0;
    bool depthPyramidBuildEnabled = false;
    unsigned localLightCount = 0;
    unsigned lightListOverflowCount = 0;
    unsigned shadowViewCount = 0;
    unsigned shadowAtlasCapacity = 0;
    unsigned shadowAtlasOverflowCount = 0;
    unsigned reflectionProbeCount = 0;
    std::array<unsigned, 8> materialClassCounts{};
    bool shadowsEnabled = false;
    bool environmentMapEnabled = false;
    double effectiveExposure = 1.0;
    bool depthPyramidOcclusion = false;
    bool gpuDrivenVisibility = false;
    bool gpuVisibilityValidated = false;
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
    double cpuGraphCompileMs = 0.0;
    unsigned graphPassCount = 0;
    unsigned graphResourceCount = 0;
    unsigned graphBarrierCount = 0;
    unsigned graphPhysicalAllocationCount = 0;
    std::uint64_t graphTransientRequestedBytes = 0;
    std::uint64_t graphTransientAllocatedBytes = 0;
    std::uint64_t graphRecompileCount = 0;
    bool graphLastCompileWasResize = false;
    double assetCookMs = 0.0;
    unsigned assetRecordCount = 0;
    unsigned assetCacheHits = 0;
    unsigned assetCacheMisses = 0;
    unsigned assetRebuiltCount = 0;
    std::uint64_t sceneTriangleCount = 0;
    std::uint64_t triangleCount = 0;
};

struct RendererOverlayFrame {
    const Camera& camera;
    const RenderStats& stats;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

using RendererOverlayCallback = void (*)(void* context,
                                         const RendererOverlayFrame& frame);

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void draw(const Camera& camera, const SceneRenderList& scene, double sceneBuildMs,
                      double elapsedSeconds, double frameDeltaMs) = 0;
    [[nodiscard]] virtual RenderStats stats() const = 0;
    [[nodiscard]] virtual const RenderDeviceInfo& deviceInfo() const = 0;
};

} // namespace ve
