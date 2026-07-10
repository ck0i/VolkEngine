#pragma once

#include "renderer/vulkan/VulkanRenderer.hpp"
#include "renderer/Geometry.hpp"
#include "renderer/FrameGraph.hpp"
#include "renderer/FrameGraphTopology.hpp"
#include "renderer/GpuResourceRegistry.hpp"
#include "renderer/SceneRenderer.hpp"
#include "core/FileSystem.hpp"
#include "core/Log.hpp"
#include "renderer/ImageLoader.hpp"
#include "platform/Window.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#if VOLKENGINE_ENABLE_IMGUI
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace ve {
namespace vulkan_renderer_detail {
inline constexpr std::array<const char*, 1> kValidationLayers{"VK_LAYER_KHRONOS_validation"};
inline constexpr std::array<const char*, 1> kDeviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
inline constexpr std::size_t kInitialSceneInstanceCapacity = 2048;
inline constexpr std::uint32_t kMaterialTextureCount = 3;
#if VOLKENGINE_ENABLE_IMGUI
inline constexpr double kImGuiDiagnosticsRefreshIntervalSeconds = 0.25;
#endif

inline double bytesToMiB(const std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
[[nodiscard]] inline constexpr VkImageLayout depthAttachmentLayout(const FrameGraphAccess access) noexcept {
    return access == FrameGraphAccess::Read
        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
}
[[nodiscard]] inline constexpr bool imageSyncStateRequiresBarrier(
    const VkImageLayout currentLayout, const VkPipelineStageFlags2 currentStage, const VkAccessFlags2 currentAccess,
    const VkImageLayout newLayout, const VkPipelineStageFlags2 newStage, const VkAccessFlags2 newAccess,
    const bool forceMemoryDependency) noexcept {
    return forceMemoryDependency || currentLayout != newLayout || currentStage != newStage || currentAccess != newAccess;
}


enum class FrameSubmissionProgress : std::uint8_t {
    BeforeAcquire,
    ImageAcquired,
    CommandsSubmitted,
};

[[nodiscard]] inline constexpr bool frameFailureRequiresAcquireRecovery(
    const FrameSubmissionProgress progress) noexcept {
    return progress == FrameSubmissionProgress::ImageAcquired;
}
struct TonemapPushConstants {
    float exposure = 1.0f;
    std::uint32_t applySrgbOetf = 1U;
};
static_assert(sizeof(TonemapPushConstants) == 8, "Tonemap push constants must match tonemap.frag");

[[nodiscard]] inline bool isSrgbSwapchainFormat(const VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool isUnormSwapchainFormat(const VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

struct GpuVertex {
    Vec3 position;
    Vec2 uv;
    std::array<std::int16_t, 4> normal;
    std::array<std::int16_t, 4> tangent;
};
static_assert(sizeof(GpuVertex) == 36, "GpuVertex layout is part of the Vulkan vertex input contract");
static_assert(offsetof(GpuVertex, position) == 0, "GpuVertex position offset changed");
static_assert(offsetof(GpuVertex, uv) == 12, "GpuVertex uv offset changed");
static_assert(offsetof(GpuVertex, normal) == 20, "GpuVertex normal offset changed");
static_assert(offsetof(GpuVertex, tangent) == 28, "GpuVertex tangent offset changed");

struct PipelineCacheHeader {
    std::uint32_t headerSize = 0;
    std::uint32_t headerVersion = 0;
    std::uint32_t vendorID = 0;
    std::uint32_t deviceID = 0;
    std::array<std::uint8_t, VK_UUID_SIZE> pipelineCacheUUID{};
};

struct FrustumPlane {
    Vec3 normal{};
    float distance = 0.0f;
};

using Frustum = std::array<FrustumPlane, 6>;

enum class FrustumSphereClassification : std::uint8_t {
    Outside,
    Intersects,
    Inside,
};

enum class SceneMeshBatchId : std::uint8_t {
    Cube,
    SphereHigh,
    SphereMedium,
    SphereLow,
    GroundPlane,
    ImportedModel
};

inline constexpr std::array<SceneMeshBatchId, 6> kSceneMeshBatchOrder{
    SceneMeshBatchId::Cube,
    SceneMeshBatchId::SphereHigh,
    SceneMeshBatchId::SphereMedium,
    SceneMeshBatchId::SphereLow,
    SceneMeshBatchId::GroundPlane,
    SceneMeshBatchId::ImportedModel,
};

inline std::size_t sceneMeshBatchIndex(const SceneMeshBatchId batch) {
    for (std::size_t index = 0; index < kSceneMeshBatchOrder.size(); ++index) {
        if (kSceneMeshBatchOrder[index] == batch) {
            return index;
        }
    }
    throw std::runtime_error("Unknown scene mesh batch id");
}

inline std::size_t sceneMeshBatchIndex(const SceneMeshId mesh) {
    switch (mesh) {
    case SceneMeshId::Cube:
        return sceneMeshBatchIndex(SceneMeshBatchId::Cube);
    case SceneMeshId::Sphere:
        return sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh);
    case SceneMeshId::GroundPlane:
        return sceneMeshBatchIndex(SceneMeshBatchId::GroundPlane);
    case SceneMeshId::ImportedModel:
        return sceneMeshBatchIndex(SceneMeshBatchId::ImportedModel);
    }
    throw std::runtime_error("Unknown scene mesh id");
}

template <typename RetiredSet>
inline void replaceFenceReferences(std::vector<RetiredSet>& retiredSets,
                                   const VkFence oldFence,
                                   const VkFence replacementFence) noexcept {
    if (oldFence == VK_NULL_HANDLE) {
        return;
    }
    for (RetiredSet& retired : retiredSets) {
        for (VkFence& completionFence : retired.completionFences) {
            if (completionFence == oldFence) {
                completionFence = replacementFence;
            }
        }
    }
}
inline bool sameGridRange(const SceneGridRange& lhs, const SceneGridRange& rhs) noexcept {
    return lhs.firstItem == rhs.firstItem &&
           lhs.rows == rhs.rows &&
           lhs.columns == rhs.columns &&
           lhs.valid == rhs.valid;
}

inline bool sameMatrix(const Mat4& lhs, const Mat4& rhs) noexcept {
    return lhs.m == rhs.m;
}


inline Vec4 matrixRow(const Mat4& matrix, const std::size_t row) {
    return Vec4{matrix.m[row], matrix.m[4U + row], matrix.m[8U + row], matrix.m[12U + row]};
}

inline FrustumPlane normalizedPlane(const Vec4 plane) {
    const Vec3 normal{plane.x, plane.y, plane.z};
    const float invLength = 1.0f / std::max(length(normal), 0.000001f);
    return FrustumPlane{normal * invLength, plane.w * invLength};
}

inline Vec4 addVec4(const Vec4 a, const Vec4 b) {
    return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

inline Vec4 subtractVec4(const Vec4 a, const Vec4 b) {
    return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

inline Frustum extractFrustumPlanes(const Mat4& viewProjection) {
    const Vec4 r0 = matrixRow(viewProjection, 0);
    const Vec4 r1 = matrixRow(viewProjection, 1);
    const Vec4 r2 = matrixRow(viewProjection, 2);
    const Vec4 r3 = matrixRow(viewProjection, 3);
    return {{
        normalizedPlane(addVec4(r3, r0)),
        normalizedPlane(subtractVec4(r3, r0)),
        normalizedPlane(addVec4(r3, r1)),
        normalizedPlane(subtractVec4(r3, r1)),
        normalizedPlane(r2),
        normalizedPlane(subtractVec4(r3, r2)),
    }};
}

inline FrustumSphereClassification classifySphereAgainstFrustum(const Frustum& frustum, const Vec3 center, const float radius) {
    bool fullyInside = true;
    for (const FrustumPlane& plane : frustum) {
        const float signedDistance = dot(plane.normal, center) + plane.distance;
        if (signedDistance < -radius) {
            return FrustumSphereClassification::Outside;
        }
        fullyInside = fullyInside && signedDistance >= radius;
    }
    return fullyInside ? FrustumSphereClassification::Inside : FrustumSphereClassification::Intersects;
}



inline const char* capabilityName(const bool available) noexcept {
    return available ? "yes" : "no";
}

inline const char* transferUploadSyncName(const TransferUploadSyncMode mode) noexcept {
    switch (mode) {
    case TransferUploadSyncMode::SameQueueBarrier:
        return "same-queue-barrier";
    case TransferUploadSyncMode::QueueSemaphore:
        return "queue-semaphore";
    }
    return "unknown";
}

inline const char* gpuClassName(const bool discrete) noexcept {
    return discrete ? "discrete" : "integrated/other";
}

inline const char* depthPrepassModeName(const DepthPrepassMode mode) {
    switch (mode) {
    case DepthPrepassMode::Auto:
        return "auto";
    case DepthPrepassMode::ForceOn:
        return "force-on";
    case DepthPrepassMode::ForceOff:
        return "force-off";
    }
    return "unknown";
}


inline VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                             void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        logger()->error("Vulkan validation: {}", callbackData->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        logger()->warn("Vulkan validation: {}", callbackData->pMessage);
    } else {
        logger()->debug("Vulkan validation: {}", callbackData->pMessage);
    }
    return VK_FALSE;
}

inline void checkVk(const VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(static_cast<int>(result)));
    }
}

class ScopedVmaMap {
public:
    ScopedVmaMap(const VmaAllocator allocator, const VmaAllocation allocation, const char* operation)
        : allocator_(allocator), allocation_(allocation) {
        checkVk(vmaMapMemory(allocator_, allocation_, &mapped_), operation);
    }

    ~ScopedVmaMap() {
        if (mapped_ != nullptr) {
            vmaUnmapMemory(allocator_, allocation_);
        }
    }

    ScopedVmaMap(const ScopedVmaMap&) = delete;
    ScopedVmaMap& operator=(const ScopedVmaMap&) = delete;

    [[nodiscard]] void* get() const { return mapped_; }

private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
};

inline bool supportsBindlessSampledImages(const VkPhysicalDeviceVulkan12Features& features) {
    return features.descriptorIndexing == VK_TRUE &&
           features.runtimeDescriptorArray == VK_TRUE &&
           features.descriptorBindingVariableDescriptorCount == VK_TRUE &&
           features.descriptorBindingPartiallyBound == VK_TRUE &&
           features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
}

template <typename T>
inline std::uint64_t handleToUint64(T handle) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uint64_t>(handle);
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}
inline std::uint32_t bytesPerPixelEstimate(const VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 5;
    default:
        return 4;
    }
}

inline std::uint32_t mipLevelCountForExtent(VkExtent2D extent) {
    std::uint32_t levels = 1;
    std::uint32_t dimension = std::max(extent.width, extent.height);
    while (dimension > 1U) {
        dimension /= 2U;
        ++levels;
    }
    return levels;
}

[[nodiscard]] inline bool textureExtentFitsDeviceLimit(const VkExtent2D extent, const std::uint32_t maxImageDimension2D) noexcept {
    return maxImageDimension2D > 0U && extent.width <= maxImageDimension2D && extent.height <= maxImageDimension2D;
}

template <typename PendingUploads>
inline void queueReservedUploadWaitSemaphores(PendingUploads& pendingUploads, std::vector<VkSemaphore>& frameUploadWaitSemaphores) noexcept {
    for (auto& upload : pendingUploads) {
        if (upload.signalSemaphore == VK_NULL_HANDLE) {
            continue;
        }
        frameUploadWaitSemaphores.push_back(upload.signalSemaphore);
        upload.signalSemaphore = VK_NULL_HANDLE;
    }
}

inline std::uint64_t imageByteEstimate(VkExtent2D extent, const VkFormat format, const std::uint32_t mipLevels = 1) {
    const std::uint32_t bytesPerPixel = bytesPerPixelEstimate(format);
    std::uint64_t total = 0;
    for (std::uint32_t level = 0; level < mipLevels; ++level) {
        total += static_cast<std::uint64_t>(extent.width) * extent.height * bytesPerPixel;
        extent.width = std::max(1U, extent.width / 2U);
        extent.height = std::max(1U, extent.height / 2U);
    }
    return total;
}


inline VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    return createInfo;
}

inline VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits stage, VkShaderModule module) {
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage = stage;
    info.module = module;
    info.pName = "main";
    return info;
}

inline std::uint32_t clampImageCount(const VkSurfaceCapabilitiesKHR& capabilities) {
    std::uint32_t imageCount = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount > 0U && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }
    return imageCount;
}

inline std::string_view presentModeName(const VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:
        return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "FIFO_RELAXED";
    default:
        return "UNKNOWN";
    }
}

inline std::array<std::filesystem::path, 5> shaderSpirvPaths(const std::filesystem::path& shaderDirectory) {
    return {
        shaderDirectory / "scene.vert.spv",
        shaderDirectory / "scene.frag.spv",
        shaderDirectory / "tonemap.vert.spv",
        shaderDirectory / "tonemap.frag.spv",
        shaderDirectory / "scene_depth.vert.spv",
    };
}
} // namespace vulkan_renderer_detail

using namespace vulkan_renderer_detail;

class VulkanRenderer::Impl final {
public:
    Impl(Window& window, EngineConfig config);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void draw(const Camera& camera, const SceneRenderList& scene, double sceneBuildMs,
              double elapsedSeconds, double frameDeltaMs);
    [[nodiscard]] MeshBounds meshBounds(SceneMeshId mesh) const;
    [[nodiscard]] RenderStats stats() const { return stats_; }
    [[nodiscard]] const RenderDeviceInfo& deviceInfo() const { return deviceInfo_; }
    void requestScreenshot(std::filesystem::path path);
    void armAcquireRecoverySmoke() noexcept { acquireRecoverySmokeArmed_ = true; }
    void waitIdle();

private:
    static constexpr std::size_t kMaxFramesInFlight = 2;
    static constexpr std::size_t kSceneMeshBatchCount = kSceneMeshBatchOrder.size();
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
    [[nodiscard]] static Buffer takeBuffer(Buffer& buffer) noexcept {
        Buffer taken = buffer;
        buffer = {};
        return taken;
    }

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
    [[nodiscard]] static ImageResource takeImage(ImageResource& image) noexcept {
        ImageResource taken = image;
        image = {};
        return taken;
    }


    struct GpuMesh {
        std::uint32_t indexCount = 0;
        std::uint32_t firstIndex = 0;
        std::int32_t vertexOffset = 0;
    };


    struct MeshUpload {
        Buffer vertices;
        Buffer indices;
        Buffer staging;
        VkDeviceSize indexStagingOffset = 0;
        VkDeviceSize vertexSize = 0;
        VkDeviceSize indexSize = 0;
        std::array<GpuMesh, kSceneMeshBatchCount> meshes{};
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
        Buffer staging;
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
    struct FrameGraphVariant {
        FrameGraph graph;
        FrameGraphResources resources;
        FrameGraphPasses passes;
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
        Vec4 normalMatrix0;
        Vec4 normalMatrix1;
        Vec4 normalMatrix2;
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
    static_assert(sizeof(InstanceData) == 160, "InstanceData must match GLSL SceneInstance layout");
    static_assert(offsetof(InstanceData, model) == 0, "InstanceData.model offset mismatch");
    static_assert(offsetof(InstanceData, normalMatrix0) == 64, "InstanceData.normalMatrix0 offset mismatch");
    static_assert(offsetof(InstanceData, normalMatrix1) == 80, "InstanceData.normalMatrix1 offset mismatch");
    static_assert(offsetof(InstanceData, normalMatrix2) == 96, "InstanceData.normalMatrix2 offset mismatch");
    static_assert(offsetof(InstanceData, albedoRoughness) == 112, "InstanceData.albedoRoughness offset mismatch");
    static_assert(offsetof(InstanceData, emissiveMetallic) == 128, "InstanceData.emissiveMetallic offset mismatch");
    static_assert(offsetof(InstanceData, materialFlags) == 144, "InstanceData.materialFlags offset mismatch");

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
    [[nodiscard]] PipelineSet detachActivePipelineSet() noexcept;
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
    [[nodiscard]] bool resolveDepthPrepassForFrame(const SceneVisibilityPlan& visibility);
    void recordCommandBuffer(FrameResources& frame, std::uint32_t imageIndex, const SceneRenderList& renderItems, const SceneVisibilityPlan& visibility, bool useDepthPrepass, const Buffer* screenshotReadback, const FrameGraphVariant& graphVariant);
    void restoreFrameFenceAfterSubmitFailure(FrameResources& frame, std::size_t frameIndex, VkResult submitResult);
    void replaceFrameImageAvailableSemaphore(FrameResources& frame, std::size_t frameIndex);
    void recoverAcquiredFrame(FrameResources& frame, std::size_t frameIndex, std::uint32_t imageIndex,
                              const FrameImageSyncSnapshot& imageSyncSnapshot);
    [[nodiscard]] VkDeviceSize checkedSceneInstanceBufferSize(std::size_t capacity) const;
    void createFrameInstanceDataBuffer(FrameResources& frame, std::size_t frameIndex, std::size_t capacity);
    void updateFrameInstanceDataDescriptor(std::size_t frameIndex) const;
    void ensureSceneInstanceCapacity(FrameResources& frame, std::size_t frameIndex, std::size_t requiredCapacity);
    void updateUniforms(FrameResources& frame, const Camera& camera, const Mat4& viewProjection, double elapsedSeconds);
    void readBackGpuTimestamp(std::uint32_t frameIndex);
    [[nodiscard]] bool screenshotFormatSupported() const;
    void recordScreenshotCopy(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, const Buffer& readback, ImageSyncState destinationState);
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
    [[nodiscard]] VkCommandBuffer beginOneShotUploadCommands(VkCommandPool commandPool, const char* operationName) const;
    [[nodiscard]] VkCommandBuffer beginGraphicsUploadCommands() const;
    void submitGraphicsUpload(VkCommandBuffer commandBuffer, Buffer staging);
    void submitTransferUpload(VkCommandBuffer commandBuffer, Buffer staging);
    void submitUploadBatch(VkQueue queue, VkCommandPool commandPool, VkCommandBuffer commandBuffer, const char* operationName, Buffer staging, bool signalSemaphore);
    void retireCompletedUploads();
    void destroyPendingUpload(PendingUploadBatch& upload);
    void retirePendingUploadResources(PendingUploadBatch& upload);
    void destroyFrameUploadWaitSemaphores(FrameResources& frame);
    void collectPendingUploadWaitSemaphores(std::vector<VkSemaphore>& semaphores) const;
    void markUploadWaitSemaphoresQueued(FrameResources& frame) noexcept;
    [[nodiscard]] bool formatSupportsLinearMipBlit(VkFormat format) const;
    void generateMipmaps(VkCommandBuffer commandBuffer, ImageResource& image) const;
    [[nodiscard]] VkCommandBuffer beginUploadCommands() const;
    [[nodiscard]] MeshUpload stageMeshUpload(std::array<MeshData, kSceneMeshBatchCount>& meshes);
    void recordMeshUpload(VkCommandBuffer commandBuffer, const MeshUpload& upload) const;
    [[nodiscard]] const GpuMesh& meshFor(SceneMeshId mesh) const;
    class DebugLabelScope final {
    public:
        DebugLabelScope(const Impl& renderer, VkCommandBuffer commandBuffer, const char* name, const std::array<float, 4>& color) noexcept;
        ~DebugLabelScope() noexcept;

        DebugLabelScope(const DebugLabelScope&) = delete;
        DebugLabelScope& operator=(const DebugLabelScope&) = delete;
        DebugLabelScope(DebugLabelScope&&) = delete;
        DebugLabelScope& operator=(DebugLabelScope&&) = delete;

    private:
        const Impl* renderer_ = nullptr;
        VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
        bool active_ = false;
    };

    [[nodiscard]] const GpuMesh& meshForBatch(std::size_t meshIndex) const;
    [[nodiscard]] static ImageSyncState imageSyncStateFor(FrameGraphAccess access, FrameGraphUsage usage);
    [[nodiscard]] static ImageSyncState finalImageSyncStateFor(FrameGraphUsage usage);
    [[nodiscard]] FrameImageSyncSnapshot captureFrameImageSyncState(std::uint32_t imageIndex) const;
    void restoreFrameImageSyncState(std::uint32_t imageIndex, const FrameImageSyncSnapshot& snapshot);
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, std::uint32_t baseMipLevel = 0, std::uint32_t levelCount = 1) const;
    void transitionImageTracked(VkCommandBuffer cmd, VkImage image, ImageSyncState& syncState, ImageSyncState newState, VkImageAspectFlags aspect, std::uint32_t baseMipLevel = 0, std::uint32_t levelCount = 1, bool forceMemoryDependency = false) const;
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
    bool acquireRecoverySmokeArmed_ = false;
    bool acquireRecoveryFailed_ = false;
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
    ImageResource groundNormalTexture_;
    ImageResource groundOrmTexture_;
    VkSampler linearSampler_ = VK_NULL_HANDLE;
    VkSampler textureSampler_ = VK_NULL_HANDLE;
    VkSampler normalTextureSampler_ = VK_NULL_HANDLE;
    VkSampler ormTextureSampler_ = VK_NULL_HANDLE;
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
    bool autoDepthPrepassEnabled_ = false;
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
    std::array<GpuMesh, kSceneMeshBatchCount> sceneMeshes_{};
    std::array<MeshBounds, kSceneMeshBatchCount> sceneMeshBounds_{};
    std::array<std::uint32_t, kSceneMeshBatchCount> sceneMeshTriangleCounts_{};
    VkQueryPool timestampQueryPool_ = VK_NULL_HANDLE;
    bool timestampsEnabled_ = false;
    std::uint32_t timestampValidBits_ = 0;
    std::array<std::filesystem::file_time_type, 5> shaderWriteTimes_{};
    double shaderHotReloadRetryDelaySeconds_ = 0.5;
    double shaderHotReloadLastCheckSeconds_ = 0.0;
    RenderStats stats_{};
    RenderDeviceInfo deviceInfo_{};
    std::array<FrameGraphVariant, 4> frameGraphVariants_{};
    GpuResourceRegistry resourceRegistry_{};
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
        Vec3 cameraPosition{};
        Vec3 cameraForward{1.0f, 0.0f, 0.0f};
        std::uint32_t culledItemCount = 0;
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
        std::uint32_t culledItemCount = 0;
        std::uint32_t gridTileCount = 0;
        std::uint32_t gridTilesAccepted = 0;
        std::uint32_t gridTilesCulled = 0;
        std::uint32_t gridTilesIntersected = 0;
    };

    struct InstanceSortKey {
        float depth = 0.0f;
        std::uint32_t index = 0;
    };
    static_assert(sizeof(InstanceSortKey) == 8, "InstanceSortKey should stay compact for per-frame sorting");

    std::vector<VisibleSceneWork> visibleSceneWorkScratch_;
    std::array<std::vector<InstanceData>, kSceneMeshBatchCount> instanceSortScratch_;
    std::array<std::vector<InstanceSortKey>, kSceneMeshBatchCount> instanceSortKeyScratch_;
    CachedGridVisibility gridVisibilityCache_;
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT_ = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT_ = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXT_ = nullptr;
};

} // namespace ve
