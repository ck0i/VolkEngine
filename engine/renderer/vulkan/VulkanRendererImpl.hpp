#pragma once

#include "assets/ReferenceAssetPipeline.hpp"
#include "core/FileSystem.hpp"
#include "core/Log.hpp"
#include "platform/Window.hpp"
#include "renderer/FrameGraph.hpp"
#include "renderer/FrameGraphTopology.hpp"
#include "renderer/Geometry.hpp"
#include "renderer/GpuResourceRegistry.hpp"
#include "renderer/ImageLoader.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/vulkan/VulkanFrameGraphSync.hpp"
#include "renderer/vulkan/VulkanReadbackState.hpp"
#include "renderer/vulkan/VulkanRenderer.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>
#if VOLKENGINE_ENABLE_IMGUI
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
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
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace ve {
namespace vulkan_renderer_detail {
inline constexpr std::array<const char *, 1> kValidationLayers{
    "VK_LAYER_KHRONOS_validation"};
inline constexpr std::array<const char *, 1> kDeviceExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME};
inline constexpr std::size_t kInitialSceneInstanceCapacity = 2048;
inline constexpr std::uint32_t kMaterialTextureCount = 3;
#if VOLKENGINE_ENABLE_IMGUI
inline constexpr double kImGuiDiagnosticsRefreshIntervalSeconds = 0.25;
#endif

inline double bytesToMiB(const std::uint64_t bytes) {
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
[[nodiscard]] inline constexpr VkImageLayout
depthAttachmentLayout(const FrameGraphAccess access) noexcept {
  return access == FrameGraphAccess::Read
             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
             : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
}
[[nodiscard]] inline constexpr bool imageSyncStateRequiresBarrier(
    const VkImageLayout currentLayout, const VkPipelineStageFlags2 currentStage,
    const VkAccessFlags2 currentAccess, const VkImageLayout newLayout,
    const VkPipelineStageFlags2 newStage, const VkAccessFlags2 newAccess,
    const bool forceMemoryDependency) noexcept {
  return forceMemoryDependency || currentLayout != newLayout ||
         currentStage != newStage || currentAccess != newAccess;
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

enum class InstanceMaterializationPolicy : std::uint8_t {
    DirectMapped,
  FrontToBackSort,
};

[[nodiscard]] inline constexpr InstanceMaterializationPolicy
instanceMaterializationPolicy(const bool useDepthPrepass) noexcept {
  return useDepthPrepass ? InstanceMaterializationPolicy::DirectMapped
                         : InstanceMaterializationPolicy::FrontToBackSort;
}
struct TonemapPushConstants {
  float exposure = 1.0f;
  std::uint32_t applySrgbOetf = 1U;
};
static_assert(sizeof(TonemapPushConstants) == 8,
              "Tonemap push constants must match tonemap.frag");

[[nodiscard]] inline bool
isSrgbSwapchainFormat(const VkFormat format) noexcept {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
  }
}

[[nodiscard]] inline bool
isUnormSwapchainFormat(const VkFormat format) noexcept {
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
static_assert(sizeof(GpuVertex) == 36,
              "GpuVertex layout is part of the Vulkan vertex input contract");
static_assert(offsetof(GpuVertex, position) == 0,
              "GpuVertex position offset changed");
static_assert(offsetof(GpuVertex, uv) == 12, "GpuVertex uv offset changed");
static_assert(offsetof(GpuVertex, normal) == 20,
              "GpuVertex normal offset changed");
static_assert(offsetof(GpuVertex, tangent) == 28,
              "GpuVertex tangent offset changed");

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
    GroundPlane
};

inline constexpr std::array<SceneMeshBatchId, 5> kBaseSceneMeshBatchOrder{
    SceneMeshBatchId::Cube,         SceneMeshBatchId::SphereHigh,
    SceneMeshBatchId::SphereMedium, SceneMeshBatchId::SphereLow,
    SceneMeshBatchId::GroundPlane,
};

inline std::size_t sceneMeshBatchIndex(const SceneMeshBatchId batch) {
  for (std::size_t index = 0; index < kBaseSceneMeshBatchOrder.size();
       ++index) {
    if (kBaseSceneMeshBatchOrder[index] == batch) {
      return index;
    }
    }
    throw std::runtime_error("Unknown scene mesh batch id");
}

inline std::size_t baseSceneMeshBatchIndex(const MeshAssetHandle mesh) {
  if (mesh == builtin_assets::kCube)
    return sceneMeshBatchIndex(SceneMeshBatchId::Cube);
  if (mesh == builtin_assets::kSphere)
    return sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh);
  if (mesh == builtin_assets::kGroundPlane)
    return sceneMeshBatchIndex(SceneMeshBatchId::GroundPlane);
  throw std::runtime_error("Mesh is not a built-in scene mesh");
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
inline bool sameGridRange(const SceneGridRange &lhs,
                          const SceneGridRange &rhs) noexcept {
  return lhs.firstItem == rhs.firstItem && lhs.rows == rhs.rows &&
         lhs.columns == rhs.columns && lhs.valid == rhs.valid;
}

inline bool sameMatrix(const Mat4 &lhs, const Mat4 &rhs) noexcept {
  return lhs.m == rhs.m;
}

inline Vec4 matrixRow(const Mat4 &matrix, const std::size_t row) {
  return Vec4{matrix.m[row], matrix.m[4U + row], matrix.m[8U + row],
              matrix.m[12U + row]};
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

inline FrustumSphereClassification
classifySphereAgainstFrustum(const Frustum &frustum, const Vec3 center,
                             const float radius) {
  bool fullyInside = true;
  for (const FrustumPlane &plane : frustum) {
    const float signedDistance = dot(plane.normal, center) + plane.distance;
        if (signedDistance < -radius) {
            return FrustumSphereClassification::Outside;
    }
    fullyInside = fullyInside && signedDistance >= radius;
  }
  return fullyInside ? FrustumSphereClassification::Inside
                     : FrustumSphereClassification::Intersects;
}

inline const char *capabilityName(const bool available) noexcept {
  return available ? "yes" : "no";
}

inline const char *
transferUploadSyncName(const TransferUploadSyncMode mode) noexcept {
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

struct ValidationMessageState {
    std::atomic<std::uint64_t> errorCount{0};
  std::atomic<std::uint64_t> warningCount{0};
};

inline VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT,
              const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
              void *userData) noexcept {
    auto* state = static_cast<ValidationMessageState*>(userData);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (state != nullptr) {
      state->errorCount.fetch_add(1U, std::memory_order_relaxed);
    }
    try {
      logger()->error("Vulkan validation: {}", callbackData != nullptr
                                                   ? callbackData->pMessage
                                                   : "missing callback data");
    } catch (...) {
    }
  } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    if (state != nullptr) {
      state->warningCount.fetch_add(1U, std::memory_order_relaxed);
    }
    try {
      logger()->warn("Vulkan validation: {}", callbackData != nullptr
                                                  ? callbackData->pMessage
                                                  : "missing callback data");
    } catch (...) {
    }
  }
  return VK_FALSE;
}

inline void checkVk(const VkResult result, const char *operation) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                             std::to_string(static_cast<int>(result)));
  }
}

class ScopedVmaMap {
public:
  ScopedVmaMap(const VmaAllocator allocator, const VmaAllocation allocation,
               const char *operation)
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
  void *mapped_ = nullptr;
};

inline bool supportsBindlessSampledImages(
    const VkPhysicalDeviceVulkan12Features &features) {
  return features.descriptorIndexing == VK_TRUE &&
         features.runtimeDescriptorArray == VK_TRUE &&
         features.descriptorBindingVariableDescriptorCount == VK_TRUE &&
           features.descriptorBindingPartiallyBound == VK_TRUE &&
         features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
}

template <typename T> inline std::uint64_t handleToUint64(T handle) {
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
    case VK_FORMAT_D16_UNORM:
        return 2;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
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

[[nodiscard]] inline bool
textureExtentFitsDeviceLimit(const VkExtent2D extent,
                             const std::uint32_t maxImageDimension2D) noexcept {
  return maxImageDimension2D > 0U && extent.width <= maxImageDimension2D &&
         extent.height <= maxImageDimension2D;
}

template <typename PendingUploads>
inline void queueReservedUploadWaitSemaphores(
    PendingUploads &pendingUploads,
    std::vector<VkSemaphore> &frameUploadWaitSemaphores) noexcept {
  for (auto &upload : pendingUploads) {
    if (upload.signalSemaphore == VK_NULL_HANDLE) {
      continue;
        }
        frameUploadWaitSemaphores.push_back(upload.signalSemaphore);
        upload.signalSemaphore = VK_NULL_HANDLE;
  }
}

inline std::uint64_t imageByteEstimate(VkExtent2D extent, const VkFormat format,
                                       const std::uint32_t mipLevels = 1) {
  const std::uint32_t bytesPerPixel = bytesPerPixelEstimate(format);
  std::uint64_t total = 0;
  for (std::uint32_t level = 0; level < mipLevels; ++level) {
    total += static_cast<std::uint64_t>(extent.width) * extent.height *
             bytesPerPixel;
    extent.width = std::max(1U, extent.width / 2U);
    extent.height = std::max(1U, extent.height / 2U);
  }
  return total;
}

inline VkDebugUtilsMessengerCreateInfoEXT
debugMessengerCreateInfo(ValidationMessageState *state) noexcept {
  VkDebugUtilsMessengerCreateInfoEXT createInfo{
      VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    createInfo.pUserData = state;
  return createInfo;
}

inline VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits stage,
                                                   VkShaderModule module) {
  VkPipelineShaderStageCreateInfo info{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.stage = stage;
  info.module = module;
  info.pName = "main";
  return info;
}

inline std::uint32_t
clampImageCount(const VkSurfaceCapabilitiesKHR &capabilities) {
  std::uint32_t imageCount = capabilities.minImageCount + 1U;
  if (capabilities.maxImageCount > 0U &&
      imageCount > capabilities.maxImageCount) {
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

inline std::array<std::filesystem::path, 20>
shaderSpirvPaths(const std::filesystem::path &shaderDirectory) {
  return {
      shaderDirectory / "scene.vert.spv",
      shaderDirectory / "scene.frag.spv",
        shaderDirectory / "tonemap.vert.spv",
        shaderDirectory / "tonemap.frag.spv",
        shaderDirectory / "scene_depth.vert.spv",
        shaderDirectory / "scene_bindless.frag.spv",
        shaderDirectory / "scene_cull.comp.spv",
        shaderDirectory / "scene_gpu.vert.spv",
        shaderDirectory / "scene_depth_gpu.vert.spv",
        shaderDirectory / "depth_pyramid.comp.spv",
        shaderDirectory / "scene_cull_subgroup.comp.spv",
        shaderDirectory / "light_assign.comp.spv",
        shaderDirectory / "shadow.vert.spv",
        shaderDirectory / "shadow.frag.spv",
        shaderDirectory / "shadow_bindless.frag.spv",
        shaderDirectory / "shadow_opaque.vert.spv",
        shaderDirectory / "scene_depth_opaque.vert.spv",
        shaderDirectory / "scene_depth_gpu_opaque.vert.spv",
      shaderDirectory / "atmosphere.frag.spv",
        shaderDirectory / "depth_pyramid_extrema.comp.spv",
    };
}
} // namespace vulkan_renderer_detail

using namespace vulkan_renderer_detail;

class VulkanRenderer::Impl final {
public:
  Impl(Window &window, EngineConfig config,
       ReferenceAssetBundle &referenceAssets);
  ~Impl();

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  void draw(const Camera &camera, const SceneRenderList &scene,
            double sceneBuildMs, double elapsedSeconds, double frameDeltaMs);
  [[nodiscard]] MeshBounds meshBounds(MeshAssetHandle mesh) const;
  [[nodiscard]] std::array<TextureAssetHandle, 3>
  materialTextureHandles(AssetId material) const;
  [[nodiscard]] RenderStats stats() const { return stats_; }
  [[nodiscard]] const RenderDeviceInfo &deviceInfo() const {
    return deviceOwner_.info;
  }
  void requestScreenshot(std::filesystem::path path);
  void armAcquireRecoverySmoke() noexcept { acquireRecoverySmokeArmed_ = true; }
  void waitIdle();
  void reloadReferenceAssets(ReferenceAssetBundle candidate);
  void setOverlayCallback(const RendererOverlayCallback callback,
                          void *const context) noexcept {
    overlayCallback_ = callback;
    overlayContext_ = context;
  }

private:
  static constexpr std::size_t kMaxFramesInFlight = 2;
    static constexpr std::size_t kMaxDepthPyramidMipLevels = 16;
    static constexpr std::uint32_t kTimestampFrameStart = 0;
    static constexpr std::uint32_t kTimestampLightAssignmentEnd = 1;
    static constexpr std::uint32_t kTimestampCullEnd = 2;
    static constexpr std::uint32_t kTimestampShadowEnd = 3;
    static constexpr std::uint32_t kTimestampDepthEnd = 4;
    static constexpr std::uint32_t kTimestampHdrEnd = 5;
    static constexpr std::uint32_t kTimestampDepthPyramidEnd = 6;
    static constexpr std::uint32_t kTimestampFinalEnd = 7;
    static constexpr std::uint32_t kTimestampQueriesPerFrame =
        kTimestampFinalEnd + 1U;

    struct QueueFamilies {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;
    std::optional<std::uint32_t> transfer;
    [[nodiscard]] bool complete() const {
      return graphics.has_value() && present.has_value() &&
             transfer.has_value();
    }
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
        VulkanBufferSyncState syncState{};
    };
    [[nodiscard]] static Buffer takeBuffer(Buffer& buffer) noexcept {
        Buffer taken = buffer;
        buffer = {};
        return taken;
    }

    using ImageSyncState = VulkanImageSyncState;

    struct FrameImageSyncSnapshot {
        ImageSyncState depth{};
        ImageSyncState hdr{};
        ImageSyncState swapchain{};
        VulkanBufferSyncState screenshotReadback{};
    };

    struct ImageResource {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::uint32_t mipLevels = 1;
        std::uint32_t resourceId = GpuResourceRegistry::kInvalidId;
        VkDeviceSize allocationBytes = 0;
        VkDeviceSize allocationAlignment = 1;
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
    struct alignas(16) GpuCluster {
        Vec4 bounds;
        std::uint32_t indexCount = 0;
        std::uint32_t firstIndex = 0;
        std::int32_t vertexOffset = 0;
        std::uint32_t meshIndex = 0;
    };
    struct alignas(16) GpuClusterNode {
        Vec4 bounds;
        std::uint32_t left = kInvalidClusterNode;
        std::uint32_t right = kInvalidClusterNode;
        std::uint32_t cluster = kInvalidClusterNode;
        std::uint32_t padding = 0;
    };
    static_assert(sizeof(GpuCluster) == 32);
    static_assert(sizeof(GpuClusterNode) == 32);
    struct GpuMeshClusterRange {
        std::uint32_t firstCluster = 0;
        std::uint32_t clusterCount = 0;
    };
    struct MeshBatch {
        const GpuMesh* mesh = nullptr;
        std::uint32_t firstInstance = 0;
    std::uint32_t instanceCount = 0;
  };

  struct MeshUpload {
    Buffer vertices;
    Buffer indices;
        Buffer staging;
        VkDeviceSize indexStagingOffset = 0;
        VkDeviceSize vertexSize = 0;
        VkDeviceSize indexSize = 0;
        std::vector<GpuMesh> meshes;
    };

    struct FrameResources {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        Buffer sceneUniforms;
        Buffer instanceData;
        Buffer indirectCommands;
        Buffer visibleInstanceIndices;
        Buffer cullCandidates;
        Buffer cullCounters;
        Buffer cullUniforms;
        Buffer localLights;
        Buffer lightingUniforms;
        Buffer lightTileHeaders;
        Buffer lightTileIndices;
        Buffer lightListCounters;
        Buffer shadowInstanceIndices;
        Buffer shadowIndirectCommands;
        std::size_t shadowInstanceIndexCapacity = 0;
        std::uint32_t shadowAtlasOverflowCount = 0;
        std::array<std::uint32_t, kShadowAtlasSlotCount>
            shadowCommandOffsets{};
        std::array<std::uint32_t, kShadowAtlasSlotCount>
            shadowCommandCounts{};
        bool shadowHasAlphaMaskedCasters = false;
        std::uint32_t shadowViewCount = 0;
        std::vector<std::uint32_t> shadowIndexScratch;
        std::vector<std::uint32_t> shadowVisibleItemScratch;
        std::array<Mat4, kShadowAtlasSlotCount> cachedShadowViewProjection{};
        std::uint32_t cachedShadowViewCount = 0;
        std::vector<std::uint32_t> shadowCountScratch;
        std::vector<std::uint32_t> shadowCursorScratch;
        bool shadowCasterCacheValid = false;
        bool hasAlphaMaskedRenderItems = false;
        bool gpuRenderItemsChangedThisFrame = false;
        bool shadowCasterLayoutChangedThisFrame = false;
        std::size_t lightTileCapacity = 0;
        std::uint32_t reflectionProbeCount = 0;
        std::uint32_t submittedLocalLightCount = 0;
        float exposureScale = 1.0F;
        std::uint32_t completedLightListOverflowCount = 0;
        bool completedLightListCountersValid = false;
        std::size_t candidateCapacity = 0;
        std::size_t visibleInstanceIndexCapacity = 0;
        std::size_t clusterInstanceCapacity = 0;
        std::size_t instanceCapacity = 0;
        std::vector<SceneRenderItem> cachedGpuRenderItems;
        bool gpuRenderItemCacheValid = false;
        std::uint64_t cachedGpuMaterialGridContentRevision = 0;
        std::array<unsigned, kRenderMaterialClassCount>
            cachedGpuMaterialGridClassCounts{};
        std::uint64_t cachedGpuMaterialGridTriangleCount = 0;
        bool cachedGpuMaterialGridHasAlphaMaskedItems = false;
        std::vector<std::uint32_t>
            cachedGpuMaterialGridMeshPotentialCounts;
        std::uint64_t cachedGpuMaterialGridMeshContentRevision = 0;
        bool submittedOnce = false;
        bool submittedDepthPrepass = false;
        std::uint32_t submittedScenePassCount = 0;
        bool depthPyramidBuildRecorded = false;
        std::vector<std::uint32_t> expectedCullingUnitCounts;
        bool gpuVisibilityValidationPending = false;
        std::uint32_t expectedVisibleItemCount = 0;
        std::array<std::uint32_t, 3> expectedSphereLodCounts{};
        std::uint32_t submittedSceneItemCount = 0;
        std::uint32_t submittedGpuCommandCount = 0;
        bool submittedClusterCommands = true;
        std::uint32_t completedVisibleItemCount = 0;
        std::uint32_t completedVisibleCullingUnitCount = 0;
        std::array<std::uint32_t, 4> completedSphereLodCounts{};
        std::array<unsigned, kRenderMaterialClassCount>
        completedMaterialClassCounts{};
    std::uint64_t completedSceneTriangleCount = 0;
        std::uint32_t completedTestedCullingUnitCount = 0;
        std::uint32_t completedOccludedCullingUnitCount = 0;
        bool completedGpuCullCountersValid = false;
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
        FrameGraph::ResourceHandle cullCandidates;
        FrameGraph::ResourceHandle cullCounters;
        FrameGraph::ResourceHandle cullUniforms;
        FrameGraph::ResourceHandle visibleInstances;
        FrameGraph::ResourceHandle sceneInstances;
        FrameGraph::ResourceHandle indirectCommands;
        FrameGraph::ResourceHandle clusterData;
        FrameGraph::ResourceHandle meshClusterRanges;
        FrameGraph::ResourceHandle depthPyramid;
        FrameGraph::ResourceHandle screenshotReadback;
        FrameGraph::ResourceHandle localLights;
        FrameGraph::ResourceHandle lightingUniforms;
        FrameGraph::ResourceHandle lightTileHeaders;
        FrameGraph::ResourceHandle lightTileIndices;
        FrameGraph::ResourceHandle shadowAtlas;
        FrameGraph::ResourceHandle shadowInstances;
        FrameGraph::ResourceHandle shadowCommands;
        FrameGraph::ResourceHandle lightListCounters;
    };
    struct FrameGraphPasses {
        FrameGraph::PassHandle depthPrepass;
        FrameGraph::PassHandle hdrScene;
        FrameGraph::PassHandle tonemap;
        FrameGraph::PassHandle gpuCull;
        FrameGraph::PassHandle depthPyramid;
        FrameGraph::PassHandle shadows;
        FrameGraph::PassHandle lightAssignment;
        FrameGraph::PassHandle screenshotReadback;
    };
    struct FrameGraphVariant {
        FrameGraph graph;
        FrameGraphResources resources;
        FrameGraph::ExecutionState executionState;
        FrameGraphPasses passes;
    };
    struct FrameGraphRecordContext {
        Impl* renderer = nullptr;
        FrameResources* frame = nullptr;
        FrameGraphVariant* variant = nullptr;
        const MeshBatch* meshBatches = nullptr;
        const Buffer* screenshotReadback = nullptr;
        std::exception_ptr failure;
        std::uint32_t imageIndex = 0;
        std::uint32_t sceneDrawCalls = 0;
        bool useDepthPrepass = false;
        std::uint32_t gpuCullCandidateCount = 0;
        std::uint32_t lightTileColumns = 0;
        std::uint32_t lightTileRows = 0;
        std::uint32_t localLightCount = 0;
    };
    struct alignas(16) SceneUniforms {
        Mat4 viewProjection;
        Vec4 cameraPositionTime;
        Vec4 lightDirection;
        Vec4 lightColor;
        Vec4 ambientSkyColor;
    Vec4 ambientGroundColor;
    Vec4 cameraForwardTanHalfFov;
    Vec4 cameraRightAspect;
    Vec4 cameraUpAtmosphere;
  };

  struct alignas(16) InstanceData {
    Mat4 model;
    Vec4 normalMatrix0;
        Vec4 normalMatrix1;
        Vec4 normalMatrix2;
        Vec4 albedoRoughness;
        Vec4 emissiveMetallic;
        Vec4 materialFlags;
        std::array<std::uint32_t, 4> textureIndices{};
    };
    struct alignas(16) GpuCullCandidate {
        Mat4 model;
        Vec4 bounds;
        std::array<std::uint32_t, 4> metadata{};
    };
    struct alignas(16) GpuCullUniforms {
        std::array<Vec4, 6> frustumPlanes;
        Vec4 cameraPositionProjectionScale;
        Vec4 cameraForward;
        std::array<std::uint32_t, 4> counts{};
        std::array<std::uint32_t, 4> sphereLodMeshes{};
        Mat4 viewProjection;
        Vec4 depthPyramid;
    };
    struct alignas(16) GpuCullCounters {
        std::uint32_t visibleItemCount = 0;
        std::uint32_t visibleCullingUnitCount = 0;
        std::uint32_t testedCullingUnitCount = 0;
        std::uint32_t occludedCullingUnitCount = 0;
        std::array<std::uint32_t, 4> sphereLodCounts{};
    std::array<std::uint32_t, kRenderMaterialClassCount>
        visibleMaterialClassCounts{};
  };
    struct DepthPyramidPushConstants {
        std::uint32_t sourceWidth = 0;
        std::uint32_t sourceHeight = 0;
        std::uint32_t useReductionSampler = 0;
        std::uint32_t sourceHasExtrema = 0;
    };
    struct LightAssignmentPushConstants {
        std::uint32_t depthBoundsEnabled = 0;
    };
    struct alignas(16) GpuLightingUniforms {
    Mat4 viewProjection;
    RenderDirectionalLight directional;
    RenderEnvironment environment;
    Vec4 environmentDiffuseRadiance{};
    std::array<std::uint32_t, 4>
        counts{};    // lights, tile columns, tile rows, reserved
    Vec4 viewport{}; // width, height, reciprocal width, reciprocal height
    std::array<Mat4, kShadowAtlasSlotCount> shadowViewProjection{};
    std::array<Vec4, kShadowAtlasSlotCount> shadowUvScaleBias{};
        Vec4 cascadeSplits{};
        std::array<RenderReflectionProbe, kMaximumReflectionProbes>
            reflectionProbes{};
    };
    struct alignas(16) GpuLightListCounters {
        std::uint32_t overflowCount = 0;
        std::array<std::uint32_t, 3> reserved{};
    };
    struct ShadowPushConstants {
        std::uint32_t shadowViewIndex = 0;
    };
    struct PipelineSet {
        VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
        VkPipeline depthPrepass = VK_NULL_HANDLE;
        VkPipeline depthPrepassOpaque = VK_NULL_HANDLE;
        VkPipeline scene = VK_NULL_HANDLE;
        VkPipeline sceneNoPrepass = VK_NULL_HANDLE;
    VkPipeline atmosphere = VK_NULL_HANDLE;
    VkPipelineLayout tonemapLayout = VK_NULL_HANDLE;
        VkPipeline tonemap = VK_NULL_HANDLE;
        VkPipelineLayout cullLayout = VK_NULL_HANDLE;
        VkPipeline cullSubgroup = VK_NULL_HANDLE;
        VkPipelineLayout depthPyramidLayout = VK_NULL_HANDLE;
        VkPipeline depthPyramid = VK_NULL_HANDLE;
        VkPipeline cull = VK_NULL_HANDLE;
        VkPipelineLayout lightAssignmentLayout = VK_NULL_HANDLE;
        VkPipeline lightAssignment = VK_NULL_HANDLE;
        VkPipelineLayout shadowLayout = VK_NULL_HANDLE;
        VkPipeline shadow = VK_NULL_HANDLE;
        VkPipeline shadowOpaque = VK_NULL_HANDLE;
    };
    struct RetiredPipelineSet {
        PipelineSet pipelines;
    std::array<VkFence, kMaxFramesInFlight> completionFences{};
  };

  static_assert(sizeof(SceneUniforms) == 192,
                "SceneUniforms must match GLSL SceneData layout");
  static_assert(offsetof(SceneUniforms, viewProjection) == 0,
                "SceneUniforms.viewProjection offset mismatch");
  static_assert(offsetof(SceneUniforms, cameraPositionTime) == 64,
                "SceneUniforms.cameraPositionTime offset mismatch");
  static_assert(offsetof(SceneUniforms, lightDirection) == 80,
                "SceneUniforms.lightDirection offset mismatch");
  static_assert(offsetof(SceneUniforms, lightColor) == 96,
                "SceneUniforms.lightColor offset mismatch");
  static_assert(offsetof(SceneUniforms, ambientSkyColor) == 112,
                "SceneUniforms.ambientSkyColor offset mismatch");
  static_assert(offsetof(SceneUniforms, ambientGroundColor) == 128,
                "SceneUniforms.ambientGroundColor offset mismatch");
  static_assert(offsetof(SceneUniforms, cameraForwardTanHalfFov) == 144,
                "SceneUniforms.cameraForwardTanHalfFov offset mismatch");
  static_assert(offsetof(SceneUniforms, cameraRightAspect) == 160,
                "SceneUniforms.cameraRightAspect offset mismatch");
  static_assert(offsetof(SceneUniforms, cameraUpAtmosphere) == 176,
                "SceneUniforms.cameraUpAtmosphere offset mismatch");
  static_assert(sizeof(InstanceData) == 176,
                "InstanceData must match GLSL SceneInstance layout");
  static_assert(offsetof(InstanceData, model) == 0,
                "InstanceData.model offset mismatch");
  static_assert(offsetof(InstanceData, normalMatrix0) == 64,
                "InstanceData.normalMatrix0 offset mismatch");
  static_assert(offsetof(InstanceData, normalMatrix1) == 80,
                "InstanceData.normalMatrix1 offset mismatch");
  static_assert(offsetof(InstanceData, normalMatrix2) == 96,
                "InstanceData.normalMatrix2 offset mismatch");
  static_assert(offsetof(InstanceData, albedoRoughness) == 112,
                "InstanceData.albedoRoughness offset mismatch");
  static_assert(offsetof(InstanceData, emissiveMetallic) == 128,
                "InstanceData.emissiveMetallic offset mismatch");
  static_assert(offsetof(InstanceData, materialFlags) == 144,
                "InstanceData.materialFlags offset mismatch");
  static_assert(offsetof(InstanceData, textureIndices) == 160,
                "InstanceData.textureIndices offset mismatch");
  static_assert(sizeof(GpuCullCandidate) == 96,
                "GpuCullCandidate must match GLSL CullCandidate layout");
  static_assert(sizeof(GpuCullUniforms) == 240,
                  "GpuCullUniforms must match GLSL CullData layout");
    static_assert(sizeof(GpuMeshClusterRange) == 8,
                  "GpuMeshClusterRange must match GLSL MeshClusterRange layout");
    static_assert(sizeof(GpuCullCounters) == 80,
                  "GpuCullCounters must match GLSL CullCounterData layout");
    static_assert(offsetof(GpuCullCounters, sphereLodCounts) == 16,
                  "GpuCullCounters.sphereLodCounts offset mismatch");
  static_assert(offsetof(GpuCullCounters, visibleMaterialClassCounts) == 32,
                "GpuCullCounters.visibleMaterialClassCounts offset mismatch");
    static_assert(sizeof(GpuLightingUniforms) == 1632,
                  "GpuLightingUniforms must match GLSL LightingData layout");
    static_assert(offsetof(GpuLightingUniforms, directional) == 64);
    static_assert(offsetof(GpuLightingUniforms, environment) == 112);
    static_assert(offsetof(GpuLightingUniforms,
                           environmentDiffuseRadiance) == 160);
    static_assert(offsetof(GpuLightingUniforms, counts) == 176);
    static_assert(offsetof(GpuLightingUniforms, viewport) == 192);
    static_assert(offsetof(GpuLightingUniforms, shadowViewProjection) == 208);
    static_assert(offsetof(GpuLightingUniforms, shadowUvScaleBias) == 1232);
    static_assert(offsetof(GpuLightingUniforms, cascadeSplits) == 1488);
    static_assert(offsetof(GpuLightingUniforms, reflectionProbes) == 1504);
    static_assert(sizeof(GpuLightListCounters) == 16);
    static_assert(sizeof(ShadowPushConstants) == 4);
    static_assert(sizeof(LightAssignmentPushConstants) == 4);

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
    void realizeFrameGraphResources();
    void createShadowResources();
    void createSampler();
    void createTextureResources();
    void createEnvironmentResources();
    [[nodiscard]] const ReferenceAssetBundle& referenceAssets();
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
    void createDepthPyramidDescriptorSets();
    void updateDepthPyramidDescriptors() const;
    void createTimestampQueries();
    void createFrameGraph(bool resizeRecompile);
    void createImGui();
    void shutdownImGui();
    void beginImGuiFrame(const Camera &camera, double frameDeltaMs);
  void renderImGui(VkCommandBuffer commandBuffer) const;
  struct SceneVisibilityPlan;

  [[nodiscard]] bool deviceExtensionAvailable(VkPhysicalDevice device,
                                              const char *extensionName) const;
  void cleanupSwapchain();
  void recreateSwapchain();
  [[nodiscard]] SceneVisibilityPlan
  planSceneVisibility(const Camera &camera, const Mat4 &projection,
                      const Mat4 &viewProjection,
                      const SceneRenderList &renderItems);
  [[nodiscard]] bool
  resolveDepthPrepassForFrame(const SceneVisibilityPlan &visibility);
  [[nodiscard]] static bool createGraphResource(
      void *context, FrameGraph::ResourceHandle resource,
      const FrameGraph::ResourceDesc &desc,
      const FrameGraph::TransientAllocation &allocation) noexcept;
  void recordCullGraphPass(const FrameGraphRecordContext &context,
                           const FrameGraph::PassDesc &desc,
                           const FrameGraph::PassResources &resources);
  void
  recordLightAssignmentGraphPass(const FrameGraphRecordContext &context,
                                 const FrameGraph::PassDesc &desc,
                                 const FrameGraph::PassResources &resources);
  void recordShadowGraphPass(const FrameGraphRecordContext &context,
                             const FrameGraph::PassDesc &desc,
                             const FrameGraph::PassResources &resources);
  [[nodiscard]] static bool
  transitionGraphResource(void *context,
                          const FrameGraph::BarrierIntent &intent) noexcept;
  [[nodiscard]] static bool
  executeGraphPass(void *context, FrameGraph::PassHandle pass,
                   const FrameGraph::PassDesc &desc,
                   const FrameGraph::PassResources &resources) noexcept;
  [[nodiscard]] static bool retireGraphResource(
      void *context, FrameGraph::ResourceHandle resource,
      const FrameGraph::ResourceDesc &desc,
      const FrameGraph::TransientAllocation &allocation) noexcept;
  void recordDepthPyramidGraphPass(const FrameGraphRecordContext &context,
                                   const FrameGraph::PassDesc &desc,
                                   const FrameGraph::PassResources &resources);
  void recordSceneBatches(const FrameGraphRecordContext &context) const;
  void recordDepthGraphPass(const FrameGraphRecordContext &context,
                            const FrameGraph::PassDesc &desc,
                            const FrameGraph::PassResources &resources);
  void recordHdrGraphPass(const FrameGraphRecordContext &context,
                          const FrameGraph::PassDesc &desc,
                          const FrameGraph::PassResources &resources);
  void recordTonemapGraphPass(const FrameGraphRecordContext &context,
                              const FrameGraph::PassDesc &desc,
                              const FrameGraph::PassResources &resources);
  void recordScreenshotGraphPass(const FrameGraphRecordContext &context,
                                 const FrameGraph::PassDesc &desc,
                                 const FrameGraph::PassResources &resources);
  void applyGraphTransition(const FrameGraphRecordContext &context,
                            const FrameGraph::BarrierIntent &intent);
  void recordCommandBuffer(FrameResources &frame, std::uint32_t imageIndex,
                           const SceneRenderList &renderItems,
                           const SceneVisibilityPlan &visibility,
                           bool useDepthPrepass,
                           const Buffer *screenshotReadback,
                           FrameGraphVariant &graphVariant);
  void updateFrameVisibleInstanceDescriptor(std::size_t frameIndex) const;
  void restoreFrameFenceAfterSubmitFailure(FrameResources &frame,
                                           std::size_t frameIndex,
                                           VkResult submitResult);
  void replaceFrameImageAvailableSemaphore(FrameResources &frame,
                                           std::size_t frameIndex);
  void recoverAcquiredFrame(FrameResources &frame, std::size_t frameIndex,
                            std::uint32_t imageIndex,
                            const FrameImageSyncSnapshot &imageSyncSnapshot);
  [[nodiscard]] VkDeviceSize
  checkedSceneInstanceBufferSize(std::size_t capacity) const;
  void createFrameInstanceDataBuffer(FrameResources &frame,
                                     std::size_t frameIndex,
                                     std::size_t capacity);
  [[nodiscard]] InstanceData instanceDataFor(const SceneRenderItem &item) const;
  void updateFrameInstanceDataDescriptor(std::size_t frameIndex) const;
  void ensureSceneInstanceCapacity(FrameResources &frame,
                                   std::size_t frameIndex,
                                   std::size_t requiredCapacity);
  void updateFrameCullDescriptors(std::size_t frameIndex) const;
  void updateFrameLightingDescriptors(std::size_t frameIndex) const;
  void ensureLightTileCapacity(FrameResources &frame, std::size_t frameIndex,
                               std::size_t requiredTileCount);
  void prepareLighting(FrameResources &frame, std::size_t frameIndex,
                       const Camera &camera, const SceneRenderList &renderItems,
                       const Mat4 &viewProjection);
  void recordLightAssignment(VkCommandBuffer commandBuffer,
                             std::uint32_t tileColumns,
                             std::uint32_t tileRows,
                             bool depthBoundsEnabled) const;
  void ensureShadowCasterCapacity(FrameResources &frame, std::size_t frameIndex,
                                  std::size_t requiredInstanceCount);
  void prepareShadowCasters(FrameResources &frame, std::size_t frameIndex,
                            const Camera &camera,
                            const SceneRenderList &renderItems);
  void validateLightLists(FrameResources &frame) const;
  void ensureGpuVisibilityCapacity(FrameResources &frame,
                                   std::size_t frameIndex,
                                   std::size_t candidateCount,
                                   std::size_t clusterInstanceCapacity);
  [[nodiscard]] SceneVisibilityPlan
  prepareGpuVisibility(FrameResources &frame, std::size_t frameIndex,
                       const Camera &camera, const Mat4 &projection,
                       const Mat4 &viewProjection,
                       const SceneRenderList &renderItems);
  void recordGpuCull(VkCommandBuffer commandBuffer,
                     std::uint32_t candidateCount) const;
  void validateGpuVisibility(FrameResources &frame) const;
  void updateUniforms(FrameResources &frame, const Camera &camera,
                      const Mat4 &viewProjection, double elapsedSeconds);
  void readBackGpuTimestamp(std::uint32_t frameIndex);
  [[nodiscard]] bool screenshotFormatSupported() const;
  void recordScreenshotCopy(VkCommandBuffer commandBuffer,
                            std::uint32_t imageIndex, const Buffer &readback);
  void writeScreenshotPpm(const Buffer &readback, VkExtent2D extent,
                          VkFormat format,
                          const std::filesystem::path &path) const;

  [[nodiscard]] bool validationLayerAvailable() const;
  [[nodiscard]] bool
  instanceExtensionAvailable(const char *extensionName) const;
  [[nodiscard]] std::vector<const char *> requiredInstanceExtensions() const;
  [[nodiscard]] std::filesystem::path pipelineCachePath() const;
  [[nodiscard]] std::vector<std::byte> loadPipelineCacheData() const;
  [[nodiscard]] bool
  pipelineCacheDataMatchesDevice(const std::vector<std::byte> &data) const;
  [[nodiscard]] DeviceSuitability
  evaluateDeviceSuitability(VkPhysicalDevice device) const;
  [[nodiscard]] QueueFamilies findQueueFamilies(VkPhysicalDevice device) const;
  [[nodiscard]] SwapchainSupport
  querySwapchainSupport(VkPhysicalDevice device) const;
  [[nodiscard]] VkSurfaceFormatKHR
  chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats) const;
  [[nodiscard]] VkPresentModeKHR
  choosePresentMode(const std::vector<VkPresentModeKHR> &presentModes) const;
  [[nodiscard]] VkExtent2D
  chooseExtent(const VkSurfaceCapabilitiesKHR &capabilities) const;
  [[nodiscard]] VkFormat findDepthFormat() const;
  [[nodiscard]] VkFormat findShadowDepthFormat() const;
  [[nodiscard]] VkFormat findHdrFormat() const;

  [[nodiscard]] Buffer
  createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags properties,
               bool sharedGraphicsTransfer = false,
               VmaAllocationCreateFlags hostAccessFlags =
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
  void destroyBuffer(Buffer &buffer);
  void destroyMeshUpload(MeshUpload &upload);
  [[nodiscard]] ImageResource createImage(VkExtent2D extent, VkFormat format,
                                          VkImageUsageFlags usage,
                                          VkImageAspectFlags aspectFlags,
                                          std::uint32_t mipLevels = 1);
  void destroyImage(ImageResource &image);
  [[nodiscard]] VkImageView createImageView(VkImage image, VkFormat format,
                                            VkImageAspectFlags aspectFlags,
                                            std::uint32_t mipLevels = 1) const;
  [[nodiscard]] VkShaderModule
  createShaderModule(const std::filesystem::path &path) const;
  [[nodiscard]] VkCommandBuffer
  beginOneShotUploadCommands(VkCommandPool commandPool,
                             const char *operationName) const;
  [[nodiscard]] VkCommandBuffer beginGraphicsUploadCommands() const;
  void submitGraphicsUpload(VkCommandBuffer commandBuffer, Buffer staging);
  void submitTransferUpload(VkCommandBuffer commandBuffer, Buffer staging);
  void submitUploadBatch(VkQueue queue, VkCommandPool commandPool,
                         VkCommandBuffer commandBuffer,
                         const char *operationName, Buffer staging,
                         bool signalSemaphore);
  void retireCompletedUploads();
  void destroyPendingUpload(PendingUploadBatch &upload);
  void retirePendingUploadResources(PendingUploadBatch &upload);
  void destroyFrameUploadWaitSemaphores(FrameResources &frame);
  void collectPendingUploadWaitSemaphores(
      std::vector<VkSemaphore> &semaphores) const;
  void markUploadWaitSemaphoresQueued(FrameResources &frame) noexcept;
  [[nodiscard]] bool formatSupportsLinearMipBlit(VkFormat format) const;
  void generateMipmaps(VkCommandBuffer commandBuffer,
                       ImageResource &image) const;
  [[nodiscard]] VkCommandBuffer beginUploadCommands() const;
  [[nodiscard]] MeshUpload stageMeshUpload(std::vector<MeshData> &meshes);
  void recordMeshUpload(VkCommandBuffer commandBuffer,
                        const MeshUpload &upload) const;
  [[nodiscard]] const GpuMesh &meshFor(MeshAssetHandle mesh) const;
  [[nodiscard]] std::size_t meshBatchIndex(MeshAssetHandle mesh) const;
  class DebugLabelScope final {
  public:
    DebugLabelScope(const Impl &renderer, VkCommandBuffer commandBuffer,
                    const char *name,
                    const std::array<float, 4> &color) noexcept;
    ~DebugLabelScope() noexcept;

    DebugLabelScope(const DebugLabelScope &) = delete;
        DebugLabelScope& operator=(const DebugLabelScope&) = delete;
        DebugLabelScope(DebugLabelScope&&) = delete;
        DebugLabelScope& operator=(DebugLabelScope&&) = delete;

    private:
        const Impl* renderer_ = nullptr;
        VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
        bool active_ = false;
  };

  [[nodiscard]] const GpuMesh &meshForBatch(std::size_t meshIndex) const;
  [[nodiscard]] static ImageSyncState imageSyncStateFor(FrameGraphAccess access,
                                                        FrameGraphUsage usage);
  [[nodiscard]] static ImageSyncState
  finalImageSyncStateFor(FrameGraphUsage usage);
  [[nodiscard]] FrameImageSyncSnapshot
  captureFrameImageSyncState(std::uint32_t imageIndex) const;
  void restoreFrameImageSyncState(std::uint32_t imageIndex,
                                  const FrameImageSyncSnapshot &snapshot);
  void transitionImage(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout oldLayout, VkImageLayout newLayout,
                       VkImageAspectFlags aspect,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                       std::uint32_t baseMipLevel = 0,
                       std::uint32_t levelCount = 1) const;
  void transitionImageTracked(VkCommandBuffer cmd, VkImage image,
                              ImageSyncState &syncState,
                              ImageSyncState newState,
                              VkImageAspectFlags aspect,
                              std::uint32_t baseMipLevel = 0,
                              std::uint32_t levelCount = 1,
                              bool forceMemoryDependency = false) const;
  void setObjectName(VkObjectType objectType, std::uint64_t objectHandle,
                     std::string_view name) const;
  void beginDebugLabel(VkCommandBuffer commandBuffer, const char *name,
                       const std::array<float, 4> &color) const;
  void endDebugLabel(VkCommandBuffer commandBuffer) const;

  Window &window_;
    EngineConfig config_;
    struct DeviceOwner {
        bool validationEnabled = false;
        bool memoryBudgetEnabled = false;
        bool debugUtilsEnabled = false;
        vulkan_renderer_detail::ValidationMessageState validationMessages{};
        bool multiDrawIndirectEnabled = false;
        bool drawIndirectFirstInstanceEnabled = false;
        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        VkQueue presentQueue = VK_NULL_HANDLE;
        VkQueue transferQueue = VK_NULL_HANDLE;
        QueueFamilies queueFamilies{};
        VkPhysicalDeviceProperties physicalDeviceProperties{};
        VkPhysicalDeviceMemoryProperties physicalDeviceMemoryProperties{};
        VmaAllocator allocator = VK_NULL_HANDLE;
        RenderDeviceInfo info{};
        PFN_vkSetDebugUtilsObjectNameEXT setDebugObjectName = nullptr;
        PFN_vkCmdBeginDebugUtilsLabelEXT beginDebugLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT endDebugLabel = nullptr;
    };
    DeviceOwner deviceOwner_;

    bool indirectSceneDrawsEnabled_ = false;
    bool acquireRecoverySmokeArmed_ = false;
    bool acquireRecoveryFailed_ = false;
    VulkanReadbackState<Buffer> readback_;

    struct SwapchainOwner {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR handle = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        std::vector<VkImage> images;
        std::vector<VkImageView> imageViews;
        std::vector<ImageSyncState> imageStates;
        std::vector<std::uint32_t> resourceIds;
        std::vector<VkSemaphore> renderFinishedSemaphores;
        std::uint32_t minimumImageCount = 0;
  };
  SwapchainOwner swapchainOwner_;

  struct FrameOwner {
    VkCommandPool graphicsCommandPool = VK_NULL_HANDLE;
    VkCommandPool transferCommandPool = VK_NULL_HANDLE;
        std::array<FrameResources, kMaxFramesInFlight> frames{};
        std::vector<PendingUploadBatch> pendingUploads;
        std::vector<VkSemaphore> pendingUploadWaitSemaphores;
        std::vector<VkSemaphore> submitWaitSemaphores;
        std::vector<VkPipelineStageFlags> submitWaitStages;
        std::size_t currentFrame = 0;
        VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
        bool timestampsEnabled = false;
        std::uint32_t timestampValidBits = 0;
  };
  FrameOwner frameOwner_;

  struct MaterialTextureBinding {
    AssetId material;
    std::array<TextureAssetHandle,
               vulkan_renderer_detail::kMaterialTextureCount>
        textures{};
  };

  struct ResourceOwner {
        ImageResource depth;
        ImageResource shadowAtlas;
        ImageResource environmentMap;
        Vec4 environmentDiffuseRadiance{};
        std::vector<TextureRole> materialTextureRoles;
        ImageResource depthPyramid;
        std::vector<VkImageView> depthPyramidMipViews;
        bool depthPyramidValid = false;
        bool depthPyramidExtremaEnabled = false;
        ImageResource hdr;
        std::vector<ImageResource> materialTextures;
    std::array<std::size_t, vulkan_renderer_detail::kMaterialTextureCount>
        referenceMaterialTextureIndices{};
    std::vector<MaterialTextureBinding> materialTextureBindings;
    std::uint32_t materialDescriptorCapacity =
        vulkan_renderer_detail::kMaterialTextureCount;
    bool bindlessMaterialsEnabled = false;
    VkSampler linearSampler = VK_NULL_HANDLE;
    VkSampler depthReductionSampler = VK_NULL_HANDLE;
        VkSampler shadowSampler = VK_NULL_HANDLE;
        VkSampler environmentSampler = VK_NULL_HANDLE;
        bool depthReductionSamplerEnabled = false;
        VkSampler textureSampler = VK_NULL_HANDLE;
        VkSampler normalTextureSampler = VK_NULL_HANDLE;
        VkSampler ormTextureSampler = VK_NULL_HANDLE;
        bool samplerAnisotropyEnabled = false;
        float maxSamplerAnisotropy = 1.0f;
        VkDescriptorSetLayout sceneSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout tonemapSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout depthPyramidSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout cullSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout lightingSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, kMaxFramesInFlight> sceneDescriptorSets{};
        VkDescriptorSet tonemapDescriptorSet = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, kMaxDepthPyramidMipLevels>
        depthPyramidDescriptorSets{};
    std::array<VkDescriptorSet, kMaxFramesInFlight> cullDescriptorSets{};
    std::array<VkDescriptorSet, kMaxFramesInFlight> lightingDescriptorSets{};
    ReferenceAssetBundle *referenceAssets = nullptr;
    Buffer sceneVertexBuffer;
    Buffer clusterData;
    Buffer clusterHierarchy;
        Buffer meshClusterRanges;
        std::vector<GpuMeshClusterRange> sceneMeshClusterRanges;
        std::vector<GpuCluster> sceneClusters;
        std::vector<GpuClusterNode> sceneClusterHierarchy;
        std::vector<std::uint32_t> sceneClusterRoots;
        Buffer sceneIndexBuffer;
        std::vector<GpuMesh> sceneMeshes;
        std::vector<MeshBounds> sceneMeshBounds;
        std::vector<std::uint32_t> sceneMeshTriangleCounts;
        GpuResourceRegistry registry;
    };
    ResourceOwner resourceOwner_;

    struct PipelineOwner {
        VkPipelineCache cache = VK_NULL_HANDLE;
        VkPipelineLayout sceneLayout = VK_NULL_HANDLE;
        VkPipeline depthPrepass = VK_NULL_HANDLE;
        VkPipeline depthPrepassOpaque = VK_NULL_HANDLE;
        VkPipeline scene = VK_NULL_HANDLE;
        VkPipelineLayout depthPyramidLayout = VK_NULL_HANDLE;
        VkPipeline depthPyramid = VK_NULL_HANDLE;
        VkPipeline cullSubgroup = VK_NULL_HANDLE;
        VkPipeline sceneNoPrepass = VK_NULL_HANDLE;
    VkPipeline atmosphere = VK_NULL_HANDLE;
    VkPipelineLayout cullLayout = VK_NULL_HANDLE;
        VkPipeline cull = VK_NULL_HANDLE;
        VkPipelineLayout tonemapLayout = VK_NULL_HANDLE;
        VkPipeline tonemap = VK_NULL_HANDLE;
        VkPipelineLayout lightAssignmentLayout = VK_NULL_HANDLE;
        VkPipeline lightAssignment = VK_NULL_HANDLE;
        VkPipelineLayout shadowLayout = VK_NULL_HANDLE;
        VkPipeline shadow = VK_NULL_HANDLE;
        VkPipeline shadowOpaque = VK_NULL_HANDLE;
        std::vector<RetiredPipelineSet> retiredSets;
        bool autoDepthPrepassEnabled = false;
        std::array<std::filesystem::file_time_type, 20> shaderWriteTimes{};
        double hotReloadRetryDelaySeconds = 0.5;
        double hotReloadLastCheckSeconds = 0.0;
    };
    PipelineOwner pipelineOwner_;

    struct ImGuiOwner {
        bool initialized = false;
        std::uint32_t minimumImageCount = 0;
        std::uint32_t imageCount = 0;
        VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
        GpuResourceRegistry::Stats resourceStats{};
        std::uint64_t memoryUsageBytes = 0;
        std::uint64_t memoryBudgetBytes = 0;
        double diagnosticsRefreshSeconds = 0.0;
        bool diagnosticsValid = false;
    };
    ImGuiOwner imguiOwner_;

    struct GraphOwner {
        std::array<FrameGraphVariant, 4> variants{};
    };
  GraphOwner graphOwner_;
  RenderStats stats_{};
  struct VisibleSceneWork {
    enum class Kind : std::uint8_t { Item, HomogeneousGridTile };

    Kind kind = Kind::Item;
    std::uint32_t meshIndex = 0;
        std::uint32_t index = 0;
    };

    struct SceneVisibilityPlan {
        std::vector<std::uint32_t> meshInstanceCounts;
        std::uint32_t visibleItemCount = 0;
        std::uint64_t sceneTriangleCount = 0;
        std::array<unsigned, kRenderMaterialClassCount> materialClassCounts{};
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
        std::vector<std::vector<InstanceData>> instanceDataByMesh;
        std::vector<std::uint32_t> meshInstanceCounts;
        std::uint32_t visibleItemCount = 0;
        std::uint64_t sceneTriangleCount = 0;
        std::uint32_t culledItemCount = 0;
        std::uint32_t gridTileCount = 0;
        std::uint32_t gridTilesAccepted = 0;
        std::uint32_t gridTilesCulled = 0;
        std::uint32_t gridTilesIntersected = 0;
    std::array<unsigned, kRenderMaterialClassCount> materialClassCounts{};
  };

    struct InstanceSortKey {
    float depth = 0.0f;
    std::uint32_t index = 0;
  };
  static_assert(sizeof(InstanceSortKey) == 8,
                "InstanceSortKey should stay compact for per-frame sorting");

  std::vector<VisibleSceneWork> visibleSceneWorkScratch_;
  std::vector<MeshBatch> meshBatchScratch_;
    std::vector<std::uint32_t> meshFirstInstanceScratch_;
    std::vector<std::uint32_t> materializedInstanceCountScratch_;
    std::vector<std::uint32_t> gpuClusterCapacityScratch_;
    std::vector<std::uint32_t> gpuMeshPotentialCountScratch_;
    std::vector<std::vector<InstanceData>> instanceSortScratch_;
    std::vector<std::vector<InstanceSortKey>> instanceSortKeyScratch_;
    CachedGridVisibility gridVisibilityCache_;
    RendererOverlayCallback overlayCallback_ = nullptr;
    void *overlayContext_ = nullptr;
};

} // namespace ve
