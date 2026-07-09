#pragma once

#include "core/Config.hpp"
#include "renderer/Geometry.hpp"
#include "renderer/FrameGraph.hpp"
#include "renderer/GpuResourceRegistry.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/SceneRenderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace ve {
class Window;

class VulkanRenderer final : public IRenderer {
public:
    VulkanRenderer(Window& window, EngineConfig config);
    ~VulkanRenderer() override;

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void draw(const Camera& camera, double elapsedSeconds, double frameDeltaMs) override;
    [[nodiscard]] RenderStats stats() const override { return stats_; }
    [[nodiscard]] const RenderDeviceInfo& deviceInfo() const override { return deviceInfo_; }
    void requestScreenshot(std::filesystem::path path);
    void waitIdle();

private:
    static constexpr std::size_t kMaxFramesInFlight = 2;
    static constexpr std::size_t kSceneMeshBatchCount = 5;
    static constexpr std::uint32_t kTimestampFrameStart = 0;
    static constexpr std::uint32_t kTimestampDepthEnd = 1;
    static constexpr std::uint32_t kTimestampHdrEnd = 2;
    static constexpr std::uint32_t kTimestampFinalEnd = 3;
    static constexpr std::uint32_t kTimestampQueriesPerFrame = kTimestampFinalEnd + 1U;

    struct QueueFamilies {
        std::optional<std::uint32_t> graphics;
        std::optional<std::uint32_t> present;
        std::optional<std::uint32_t> transfer;
        [[nodiscard]] bool complete() const { return graphics.has_value() && present.has_value() && transfer.has_value(); }
    };

    struct SwapchainSupport {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    struct DeviceSuitability {
        bool suitable = true;
        std::vector<std::string> reasons;
    };

    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
        std::uint32_t resourceId = GpuResourceRegistry::kInvalidId;
    };

    struct ImageSyncState {
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 access = VK_ACCESS_2_NONE;
    };

    struct FrameImageSyncSnapshot {
        ImageSyncState depth{};
        ImageSyncState hdr{};
        ImageSyncState swapchain{};
    };

    struct ImageResource {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::uint32_t mipLevels = 1;
        std::uint32_t resourceId = GpuResourceRegistry::kInvalidId;
        ImageSyncState syncState{};
    };

    struct GpuMesh {
        std::uint32_t indexCount = 0;
        std::uint32_t firstIndex = 0;
        std::int32_t vertexOffset = 0;
    };


    struct MeshUpload {
        Buffer vertices;
        Buffer indices;
        Buffer vertexStaging;
        Buffer indexStaging;
        VkDeviceSize vertexSize = 0;
        VkDeviceSize indexSize = 0;
        GpuMesh cube;
        GpuMesh sphere;
        GpuMesh sphereMedium;
        GpuMesh sphereLow;
        GpuMesh plane;
    };

    struct FrameResources {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        Buffer sceneUniforms;
        Buffer instanceData;
        Buffer indirectCommands;
        std::size_t instanceCapacity = 0;
        bool submittedOnce = false;
        bool submittedDepthPrepass = false;
        std::uint32_t submittedScenePassCount = 0;
        std::vector<VkSemaphore> uploadWaitSemaphores;
    };

    struct PendingUploadBatch {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore signalSemaphore = VK_NULL_HANDLE;
        std::vector<Buffer> stagingBuffers;
    };
    struct FrameGraphResources {
        FrameGraph::ResourceHandle depth;
        FrameGraph::ResourceHandle hdr;
        FrameGraph::ResourceHandle swapchain;
    };
    struct FrameGraphPasses {
        FrameGraph::PassHandle depthPrepass;
        FrameGraph::PassHandle hdrScene;
        FrameGraph::PassHandle tonemap;
        FrameGraph::PassHandle screenshotReadback;
    };
    struct alignas(16) SceneUniforms {
        Mat4 viewProjection;
        Vec4 cameraPositionTime;
        Vec4 lightDirection;
        Vec4 lightColor;
        Vec4 ambientSkyColor;
        Vec4 ambientGroundColor;
    };


    struct alignas(16) InstanceData {
        Mat4 model;
        Vec4 albedoRoughness;
        Vec4 emissiveMetallic;
        Vec4 materialFlags;
    };
    struct PipelineSet {
        VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
        VkPipeline depthPrepass = VK_NULL_HANDLE;
        VkPipeline scene = VK_NULL_HANDLE;
        VkPipeline sceneNoPrepass = VK_NULL_HANDLE;
        VkPipelineLayout tonemapLayout = VK_NULL_HANDLE;
        VkPipeline tonemap = VK_NULL_HANDLE;
    };
    struct RetiredPipelineSet {
        PipelineSet pipelines;
        std::array<VkFence, kMaxFramesInFlight> completionFences{};
    };


    static_assert(sizeof(SceneUniforms) == 144, "SceneUniforms must match GLSL SceneData layout");
    static_assert(offsetof(SceneUniforms, viewProjection) == 0, "SceneUniforms.viewProjection offset mismatch");
    static_assert(offsetof(SceneUniforms, cameraPositionTime) == 64, "SceneUniforms.cameraPositionTime offset mismatch");
    static_assert(offsetof(SceneUniforms, lightDirection) == 80, "SceneUniforms.lightDirection offset mismatch");
    static_assert(offsetof(SceneUniforms, lightColor) == 96, "SceneUniforms.lightColor offset mismatch");
    static_assert(offsetof(SceneUniforms, ambientSkyColor) == 112, "SceneUniforms.ambientSkyColor offset mismatch");
    static_assert(offsetof(SceneUniforms, ambientGroundColor) == 128, "SceneUniforms.ambientGroundColor offset mismatch");
    static_assert(sizeof(InstanceData) == 112, "InstanceData must match GLSL SceneInstance layout");
    static_assert(offsetof(InstanceData, model) == 0, "InstanceData.model offset mismatch");
    static_assert(offsetof(InstanceData, albedoRoughness) == 64, "InstanceData.albedoRoughness offset mismatch");
    static_assert(offsetof(InstanceData, emissiveMetallic) == 80, "InstanceData.emissiveMetallic offset mismatch");
    static_assert(offsetof(InstanceData, materialFlags) == 96, "InstanceData.materialFlags offset mismatch");

    void createInstance();
    void createDebugMessenger();
    void loadDebugUtils();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createCommandPools();
    void createSwapchain();
    void createImageViews();
    void createDepthResources();
    void createHdrResources();
    void createSampler();
    void createTextureResources();
    void createDescriptorLayouts();
    void createPipelineCache();
    void savePipelineCache() const;
    void cleanupResources(bool persistPipelineCache) noexcept;
    void createPipelines();
    [[nodiscard]] PipelineSet buildPipelineSet();
    void destroyPipelineSet(PipelineSet& pipelines) const;
    void retireDeferredPipelineSets();
    void installPipelineSet(const PipelineSet& pipelines);
    void refreshShaderWriteTimes();
    [[nodiscard]] bool shaderFilesChanged() const;
    void pollShaderHotReload(double elapsedSeconds);
    void createFrameResources();
    void createMeshes();
    void createTonemapDescriptorSet();
    void createTimestampQueries();
    void createFrameGraph();
    void createImGui();
    void shutdownImGui();
    void beginImGuiFrame(double frameDeltaMs);
    void renderImGui(VkCommandBuffer commandBuffer) const;
    struct SceneVisibilityPlan;


    [[nodiscard]] bool deviceExtensionAvailable(VkPhysicalDevice device, const char* extensionName) const;
    void cleanupSwapchain();
    void recreateSwapchain();
    [[nodiscard]] SceneVisibilityPlan planSceneVisibility(const Camera& camera, const Mat4& projection, const Mat4& viewProjection, const SceneRenderList& renderItems);
    void recordCommandBuffer(FrameResources& frame, std::uint32_t imageIndex, const SceneRenderList& renderItems, const SceneVisibilityPlan& visibility, const Buffer* screenshotReadback);
    void restoreFrameFenceAfterSubmitFailure(FrameResources& frame, std::size_t frameIndex, VkResult submitResult);
    [[nodiscard]] VkDeviceSize checkedSceneInstanceBufferSize(std::size_t capacity) const;
    void createFrameInstanceDataBuffer(FrameResources& frame, std::size_t frameIndex, std::size_t capacity);
    void updateFrameInstanceDataDescriptor(std::size_t frameIndex) const;
    void ensureSceneInstanceCapacity(FrameResources& frame, std::size_t frameIndex, std::size_t requiredCapacity);
    void updateUniforms(FrameResources& frame, const Camera& camera, const Mat4& viewProjection, double elapsedSeconds);
    void readBackGpuTimestamp(std::uint32_t frameIndex);
    [[nodiscard]] bool screenshotFormatSupported() const;
    void recordScreenshotCopy(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, const Buffer& readback);
    void writeScreenshotPpm(const Buffer& readback, VkExtent2D extent, VkFormat format, const std::filesystem::path& path) const;

    [[nodiscard]] bool validationLayerAvailable() const;
    [[nodiscard]] bool instanceExtensionAvailable(const char* extensionName) const;
    [[nodiscard]] std::vector<const char*> requiredInstanceExtensions() const;
    [[nodiscard]] std::filesystem::path pipelineCachePath() const;
    [[nodiscard]] std::vector<std::byte> loadPipelineCacheData() const;
    [[nodiscard]] bool pipelineCacheDataMatchesDevice(const std::vector<std::byte>& data) const;
    [[nodiscard]] DeviceSuitability evaluateDeviceSuitability(VkPhysicalDevice device) const;
    [[nodiscard]] QueueFamilies findQueueFamilies(VkPhysicalDevice device) const;
    [[nodiscard]] SwapchainSupport querySwapchainSupport(VkPhysicalDevice device) const;
    [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
    [[nodiscard]] VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    [[nodiscard]] VkFormat findDepthFormat() const;

    [[nodiscard]] Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, bool sharedGraphicsTransfer = false, VmaAllocationCreateFlags hostAccessFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    void destroyBuffer(Buffer& buffer);
    void destroyMeshUpload(MeshUpload& upload);
    [[nodiscard]] ImageResource createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, std::uint32_t mipLevels = 1);
    void destroyImage(ImageResource& image);
    [[nodiscard]] VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, std::uint32_t mipLevels = 1) const;
    [[nodiscard]] VkShaderModule createShaderModule(const std::filesystem::path& path) const;
    [[nodiscard]] VkCommandBuffer beginGraphicsUploadCommands() const;
    void submitGraphicsUpload(VkCommandBuffer commandBuffer, std::vector<Buffer> stagingBuffers);
    void submitTransferUpload(VkCommandBuffer commandBuffer, std::vector<Buffer> stagingBuffers);
    void submitUploadBatch(VkQueue queue, VkCommandPool commandPool, VkCommandBuffer commandBuffer, const char* operationName, std::vector<Buffer> stagingBuffers, bool signalSemaphore);
    void retireCompletedUploads();
    void destroyPendingUpload(PendingUploadBatch& upload);
    void retirePendingUploadResources(PendingUploadBatch& upload);
    void destroyFrameUploadWaitSemaphores(FrameResources& frame);
    void collectPendingUploadWaitSemaphores(std::vector<VkSemaphore>& semaphores) const;
    void markUploadWaitSemaphoresQueued(FrameResources& frame, const std::vector<VkSemaphore>& semaphores);
    [[nodiscard]] bool formatSupportsLinearMipBlit(VkFormat format) const;
    void generateMipmaps(VkCommandBuffer commandBuffer, ImageResource& image) const;
    [[nodiscard]] VkCommandBuffer beginUploadCommands() const;
    [[nodiscard]] MeshUpload stageMeshUpload(const MeshData& cubeMesh, const MeshData& sphereMesh, const MeshData& sphereMediumMesh, const MeshData& sphereLowMesh, const MeshData& planeMesh);
    void recordMeshUpload(VkCommandBuffer commandBuffer, const MeshUpload& upload) const;
    [[nodiscard]] const GpuMesh& meshFor(SceneMeshId mesh) const;
    class DebugLabelScope final {
    public:
        DebugLabelScope(const VulkanRenderer& renderer, VkCommandBuffer commandBuffer, const char* name, const std::array<float, 4>& color) noexcept;
        ~DebugLabelScope() noexcept;

        DebugLabelScope(const DebugLabelScope&) = delete;
        DebugLabelScope& operator=(const DebugLabelScope&) = delete;
        DebugLabelScope(DebugLabelScope&&) = delete;
        DebugLabelScope& operator=(DebugLabelScope&&) = delete;

    private:
        const VulkanRenderer* renderer_ = nullptr;
        VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
        bool active_ = false;
    };

    [[nodiscard]] const GpuMesh& meshForBatch(std::size_t meshIndex) const;
    [[nodiscard]] static ImageSyncState imageSyncStateFor(FrameGraphAccess access, FrameGraphUsage usage);
    [[nodiscard]] static ImageSyncState finalImageSyncStateFor(FrameGraphUsage usage);
    [[nodiscard]] FrameImageSyncSnapshot captureFrameImageSyncState(std::uint32_t imageIndex) const;
    void restoreFrameImageSyncState(std::uint32_t imageIndex, const FrameImageSyncSnapshot& snapshot);
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, std::uint32_t baseMipLevel = 0, std::uint32_t levelCount = 1) const;
    void transitionImageTracked(VkCommandBuffer cmd, VkImage image, ImageSyncState& syncState, ImageSyncState newState, VkImageAspectFlags aspect, std::uint32_t baseMipLevel = 0, std::uint32_t levelCount = 1) const;
    void setObjectName(VkObjectType objectType, std::uint64_t objectHandle, std::string_view name) const;
    void beginDebugLabel(VkCommandBuffer commandBuffer, const char* name, const std::array<float, 4>& color) const;
    void endDebugLabel(VkCommandBuffer commandBuffer) const;

    Window& window_;
    EngineConfig config_;
    bool validationEnabled_ = false;
    bool memoryBudgetEnabled_ = false;
    bool debugUtilsEnabled_ = false;
    bool multiDrawIndirectEnabled_ = false;
    bool drawIndirectFirstInstanceEnabled_ = false;
    bool indirectSceneDrawsEnabled_ = false;
    mutable std::mutex screenshotRequestMutex_;
    bool screenshotPending_ = false;
    std::filesystem::path screenshotPath_;
    bool swapchainTransferSrcSupported_ = false;
    Buffer screenshotReadback_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue transferQueue_ = VK_NULL_HANDLE;
    QueueFamilies queueFamilies_{};
    VkPhysicalDeviceProperties physicalDeviceProperties_{};
    VkPhysicalDeviceMemoryProperties physicalDeviceMemoryProperties_{};
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<ImageSyncState> swapchainStates_;
    std::vector<std::uint32_t> swapchainResourceIds_;
    std::vector<VkSemaphore> swapchainRenderFinishedSemaphores_;
    std::uint32_t swapchainMinImageCount_ = 0;

    VkCommandPool graphicsCommandPool_ = VK_NULL_HANDLE;
    VkCommandPool transferCommandPool_ = VK_NULL_HANDLE;
    std::array<FrameResources, kMaxFramesInFlight> frames_{};
    std::vector<PendingUploadBatch> pendingUploads_;
    std::vector<VkSemaphore> pendingUploadWaitSemaphores_;
    std::vector<VkSemaphore> submitWaitSemaphores_;
    std::vector<VkPipelineStageFlags> submitWaitStages_;
    std::size_t frameIndex_ = 0;

    ImageResource depth_;
    ImageResource hdr_;
    ImageResource groundAlbedoTexture_;
    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkSampler textureSampler_ = VK_NULL_HANDLE;
    bool samplerAnisotropyEnabled_ = false;
    float maxSamplerAnisotropy_ = 1.0f;

    VkDescriptorSetLayout sceneSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kMaxFramesInFlight> sceneDescriptorSets_{};
    VkDescriptorSet tonemapDescriptorSet_ = VK_NULL_HANDLE;

    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkPipelineLayout scenePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline depthPrepassPipeline_ = VK_NULL_HANDLE;
    VkPipeline scenePipeline_ = VK_NULL_HANDLE;
    VkPipeline sceneNoPrepassPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout tonemapPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline_ = VK_NULL_HANDLE;
    std::vector<RetiredPipelineSet> retiredPipelineSets_;
    bool imguiInitialized_ = false;
    std::uint32_t imguiMinImageCount_ = 0;
    std::uint32_t imguiImageCount_ = 0;
    VkFormat imguiSwapchainFormat_ = VK_FORMAT_UNDEFINED;
    GpuResourceRegistry::Stats imguiResourceStats_{};
    std::uint64_t imguiMemoryUsageBytes_ = 0;
    std::uint64_t imguiMemoryBudgetBytes_ = 0;
    double imguiDiagnosticsRefreshSeconds_ = 0.0;
    bool imguiDiagnosticsValid_ = false;

    Buffer sceneVertexBuffer_;
    Buffer sceneIndexBuffer_;
    GpuMesh cube_;
    GpuMesh sphere_;
    GpuMesh sphereMedium_;
    GpuMesh sphereLow_;
    GpuMesh plane_;
    std::array<std::uint32_t, kSceneMeshBatchCount> sceneMeshTriangleCounts_{};
    VkQueryPool timestampQueryPool_ = VK_NULL_HANDLE;
    bool timestampsEnabled_ = false;
    std::uint32_t timestampValidBits_ = 0;
    std::array<std::filesystem::file_time_type, 4> shaderWriteTimes_{};
    double shaderHotReloadLastCheckSeconds_ = 0.0;
    RenderStats stats_{};
    RenderDeviceInfo deviceInfo_{};
    FrameGraph frameGraph_{};
    FrameGraphResources frameGraphResources_{};
    FrameGraphPasses frameGraphPasses_{};
    GpuResourceRegistry resourceRegistry_{};
    DemoSceneRenderer sceneRenderer_{};
    struct VisibleSceneWork {
        enum class Kind : std::uint8_t {
            Item,
            HomogeneousGridTile
        };

        Kind kind = Kind::Item;
        std::uint8_t meshIndex = 0;
        std::uint32_t index = 0;
    };

    struct SceneVisibilityPlan {
        std::array<std::uint32_t, kSceneMeshBatchCount> meshInstanceCounts{};
        std::uint32_t visibleItemCount = 0;
        std::uint64_t sceneTriangleCount = 0;
        std::uint32_t culledDrawCalls = 0;
        std::uint32_t gridTileCount = 0;
        std::uint32_t gridTilesCulled = 0;
        std::uint32_t gridTilesAccepted = 0;
        std::uint32_t gridTilesIntersected = 0;
        bool gridVisibilityCacheHit = false;
        std::uint32_t gridVisibilityWorkItems = 0;
        bool useGridTiles = false;
        SceneGridRange gridRange{};
        std::size_t gridItemCount = 0;
        std::size_t gridWorkBegin = 0;
        std::size_t gridWorkEnd = 0;
    };

    struct CachedGridVisibility {
        bool valid = false;
        std::uint64_t tileRevision = 0;
        SceneGridRange range{};
        std::size_t tileCount = 0;
        Mat4 viewProjection{};
        std::uint32_t workItemCount = 0;
        std::array<std::vector<InstanceData>, kSceneMeshBatchCount> instanceDataByMesh;
        std::array<std::uint32_t, kSceneMeshBatchCount> meshInstanceCounts{};
        std::uint32_t visibleItemCount = 0;
        std::uint64_t sceneTriangleCount = 0;
        std::uint32_t culledDrawCalls = 0;
        std::uint32_t gridTileCount = 0;
        std::uint32_t gridTilesAccepted = 0;
        std::uint32_t gridTilesCulled = 0;
        std::uint32_t gridTilesIntersected = 0;
    };

    std::vector<VisibleSceneWork> visibleSceneWorkScratch_;
    CachedGridVisibility gridVisibilityCache_;
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT_ = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXT_ = nullptr;
};

} // namespace ve
