#include "renderer/vulkan/VulkanRenderer.hpp"

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
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ve {
namespace {
constexpr std::array<const char*, 1> kValidationLayers{"VK_LAYER_KHRONOS_validation"};
constexpr std::array<const char*, 1> kDeviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
constexpr std::size_t kInitialSceneInstanceCapacity = 2048;
#if VOLKENGINE_ENABLE_IMGUI
constexpr double kImGuiDiagnosticsRefreshIntervalSeconds = 0.25;
#endif

double bytesToMiB(const std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

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

constexpr std::array<SceneMeshBatchId, 5> kSceneMeshBatchOrder{
    SceneMeshBatchId::Cube,
    SceneMeshBatchId::SphereHigh,
    SceneMeshBatchId::SphereMedium,
    SceneMeshBatchId::SphereLow,
    SceneMeshBatchId::GroundPlane,
};

std::size_t sceneMeshBatchIndex(const SceneMeshBatchId batch) {
    for (std::size_t index = 0; index < kSceneMeshBatchOrder.size(); ++index) {
        if (kSceneMeshBatchOrder[index] == batch) {
            return index;
        }
    }
    throw std::runtime_error("Unknown scene mesh batch id");
}

std::size_t sceneMeshBatchIndex(const SceneMeshId mesh) {
    switch (mesh) {
    case SceneMeshId::Cube:
        return sceneMeshBatchIndex(SceneMeshBatchId::Cube);
    case SceneMeshId::Sphere:
        return sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh);
    case SceneMeshId::GroundPlane:
        return sceneMeshBatchIndex(SceneMeshBatchId::GroundPlane);
    }
    throw std::runtime_error("Unknown scene mesh id");
}

bool sameGridRange(const SceneGridRange& lhs, const SceneGridRange& rhs) noexcept {
    return lhs.firstItem == rhs.firstItem &&
           lhs.rows == rhs.rows &&
           lhs.columns == rhs.columns &&
           lhs.valid == rhs.valid;
}

bool sameMatrix(const Mat4& lhs, const Mat4& rhs) noexcept {
    return lhs.m == rhs.m;
}


Vec4 matrixRow(const Mat4& matrix, const std::size_t row) {
    return Vec4{matrix.m[row], matrix.m[4U + row], matrix.m[8U + row], matrix.m[12U + row]};
}

FrustumPlane normalizedPlane(const Vec4 plane) {
    const Vec3 normal{plane.x, plane.y, plane.z};
    const float invLength = 1.0f / std::max(length(normal), 0.000001f);
    return FrustumPlane{normal * invLength, plane.w * invLength};
}

Vec4 addVec4(const Vec4 a, const Vec4 b) {
    return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

Vec4 subtractVec4(const Vec4 a, const Vec4 b) {
    return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

Frustum extractFrustumPlanes(const Mat4& viewProjection) {
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

FrustumSphereClassification classifySphereAgainstFrustum(const Frustum& frustum, const Vec3 center, const float radius) {
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


bool sphereOutsideFrustum(const Frustum& frustum, const Vec3 center, const float radius) {
    for (const FrustumPlane& plane : frustum) {
        if (dot(plane.normal, center) + plane.distance < -radius) {
            return true;
        }
    }
    return false;
}
bool resolveDepthPrepass(const DepthPrepassMode mode) {
    switch (mode) {
    case DepthPrepassMode::ForceOn:
        return true;
    case DepthPrepassMode::ForceOff:
        return false;
    }
    return false;
}

const char* capabilityName(const bool available) noexcept {
    return available ? "yes" : "no";
}

const char* transferUploadSyncName(const TransferUploadSyncMode mode) noexcept {
    switch (mode) {
    case TransferUploadSyncMode::SameQueueBarrier:
        return "same-queue-barrier";
    case TransferUploadSyncMode::QueueSemaphore:
        return "queue-semaphore";
    }
    return "unknown";
}

const char* gpuClassName(const bool discrete) noexcept {
    return discrete ? "discrete" : "integrated/other";
}

#if VOLKENGINE_ENABLE_IMGUI
const char* depthPrepassModeName(const DepthPrepassMode mode) {
    switch (mode) {
    case DepthPrepassMode::ForceOn:
        return "force-on";
    case DepthPrepassMode::ForceOff:
        return "force-off";
    }
    return "unknown";
}
#endif


VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
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

void checkVk(const VkResult result, const char* operation) {
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

bool supportsBindlessSampledImages(const VkPhysicalDeviceVulkan12Features& features) {
    return features.descriptorIndexing == VK_TRUE &&
           features.runtimeDescriptorArray == VK_TRUE &&
           features.descriptorBindingVariableDescriptorCount == VK_TRUE &&
           features.descriptorBindingPartiallyBound == VK_TRUE &&
           features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
}

template <typename T>
std::uint64_t handleToUint64(T handle) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uint64_t>(handle);
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}
std::uint32_t bytesPerPixelEstimate(const VkFormat format) {
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

std::uint32_t mipLevelCountForExtent(VkExtent2D extent) {
    std::uint32_t levels = 1;
    std::uint32_t dimension = std::max(extent.width, extent.height);
    while (dimension > 1U) {
        dimension /= 2U;
        ++levels;
    }
    return levels;
}

std::uint64_t imageByteEstimate(VkExtent2D extent, const VkFormat format, const std::uint32_t mipLevels = 1) {
    const std::uint32_t bytesPerPixel = bytesPerPixelEstimate(format);
    std::uint64_t total = 0;
    for (std::uint32_t level = 0; level < mipLevels; ++level) {
        total += static_cast<std::uint64_t>(extent.width) * extent.height * bytesPerPixel;
        extent.width = std::max(1U, extent.width / 2U);
        extent.height = std::max(1U, extent.height / 2U);
    }
    return total;
}


VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    return createInfo;
}

VkPipelineShaderStageCreateInfo shaderStage(VkShaderStageFlagBits stage, VkShaderModule module) {
    VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage = stage;
    info.module = module;
    info.pName = "main";
    return info;
}

std::uint32_t clampImageCount(const VkSurfaceCapabilitiesKHR& capabilities) {
    std::uint32_t imageCount = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount > 0U && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }
    return imageCount;
}

std::string_view presentModeName(const VkPresentModeKHR mode) {
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

std::array<std::filesystem::path, 4> shaderSpirvPaths(const std::filesystem::path& shaderDirectory) {
    return {
        shaderDirectory / "scene.vert.spv",
        shaderDirectory / "scene.frag.spv",
        shaderDirectory / "tonemap.vert.spv",
        shaderDirectory / "tonemap.frag.spv",
    };
}

std::filesystem::path uniquePipelineCacheTemporaryPath(const std::filesystem::path& path) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device random;
    const std::uint64_t salt = (static_cast<std::uint64_t>(random()) << 32U) ^ static_cast<std::uint64_t>(random());

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp.";
    temporaryPath += std::to_string(ticks);
    temporaryPath += ".";
    temporaryPath += std::to_string(salt);
    return temporaryPath;
}
}

VulkanRenderer::VulkanRenderer(Window& window, EngineConfig config)
    : window_(window), config_(std::move(config)) {
    try {
    createInstance();
    createDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();
    loadDebugUtils();
    createCommandPools();
    pendingUploads_.reserve(8);
    retiredPipelineSets_.reserve(2);
    createSwapchain();
    createImageViews();
    createDepthResources();
    createHdrResources();
    createTextureResources();
    createSampler();
    createDescriptorLayouts();
    createPipelineCache();
    createPipelines();
    createFrameResources();
    createMeshes();
    createTonemapDescriptorSet();
    createTimestampQueries();
    createFrameGraph();
    if (config_.debugOverlay) {
        createImGui();
    }
    logger()->info("Renderer enabled: dynamicRendering {} sync2 {} timestamps {} validation {} debugMarkers {} memoryBudget {} indirectSceneDraws {} imguiOverlay {} transferUploadSync {}; supported: descriptorIndexing {} bindlessSampledImages {} multiDrawIndirect {} drawIndirectFirstInstance {} samplerAnisotropy {} maxAnisotropy {:.1f} maxDrawIndirectCount {}",
                   capabilityName(deviceInfo_.dynamicRendering), capabilityName(deviceInfo_.synchronization2),
                   capabilityName(deviceInfo_.timestampQueries), capabilityName(deviceInfo_.validationEnabled),
                   capabilityName(deviceInfo_.debugMarkers), capabilityName(deviceInfo_.memoryBudget),
                   capabilityName(deviceInfo_.indirectSceneDraws), capabilityName(imguiInitialized_),
                   transferUploadSyncName(deviceInfo_.transferUploadSync), capabilityName(deviceInfo_.descriptorIndexing),
                   capabilityName(deviceInfo_.bindlessSampledImagesSupported), capabilityName(deviceInfo_.multiDrawIndirect),
                   capabilityName(deviceInfo_.drawIndirectFirstInstance), capabilityName(deviceInfo_.samplerAnisotropy),
                   deviceInfo_.maxSamplerAnisotropy, deviceInfo_.maxDrawIndirectCount);
    const GpuResourceRegistry::Stats resourceStats = resourceRegistry_.stats();
    logger()->info("Tracked GPU resources: {} live ({} buffers, {} images, {} imported), {:.2f} MiB estimated (buffers {:.2f}, owned images {:.2f}, imported images {:.2f})",
                   resourceStats.liveResources, resourceStats.buffers, resourceStats.images,
                   resourceStats.importedImages, bytesToMiB(resourceStats.bytes), bytesToMiB(resourceStats.bufferBytes),
                   bytesToMiB(resourceStats.ownedImageBytes), bytesToMiB(resourceStats.importedImageBytes));
    } catch (...) {
        cleanupResources(false);
        throw;
    }
}

VulkanRenderer::~VulkanRenderer() {
    cleanupResources(true);
}

void VulkanRenderer::cleanupResources(const bool persistPipelineCache) noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    shutdownImGui();

    for (PendingUploadBatch& upload : pendingUploads_) {
        destroyPendingUpload(upload);
    }
    pendingUploads_.clear();

    if (timestampQueryPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, timestampQueryPool_, nullptr);
        timestampQueryPool_ = VK_NULL_HANDLE;
    }

    destroyBuffer(screenshotReadback_);
    destroyBuffer(sceneIndexBuffer_);
    destroyBuffer(sceneVertexBuffer_);
    destroyImage(groundAlbedoTexture_);

    for (FrameResources& frame : frames_) {
        destroyFrameUploadWaitSemaphores(frame);
        destroyBuffer(frame.sceneUniforms);
        destroyBuffer(frame.instanceData);
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
            frame.imageAvailable = VK_NULL_HANDLE;
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame.inFlight, nullptr);
            frame.inFlight = VK_NULL_HANDLE;
        }
        destroyBuffer(frame.indirectCommands);
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, frame.commandPool, nullptr);
            frame.commandPool = VK_NULL_HANDLE;
        }
    }

    cleanupSwapchain();
    for (RetiredPipelineSet& retired : retiredPipelineSets_) {
        destroyPipelineSet(retired.pipelines);
    }
    retiredPipelineSets_.clear();


    if (tonemapPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, tonemapPipeline_, nullptr);
        tonemapPipeline_ = VK_NULL_HANDLE;
    }
    if (tonemapPipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, tonemapPipelineLayout_, nullptr);
        tonemapPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (depthPrepassPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, depthPrepassPipeline_, nullptr);
        depthPrepassPipeline_ = VK_NULL_HANDLE;
    }
    if (scenePipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, scenePipeline_, nullptr);
        scenePipeline_ = VK_NULL_HANDLE;
    }
    if (sceneNoPrepassPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, sceneNoPrepassPipeline_, nullptr);
        sceneNoPrepassPipeline_ = VK_NULL_HANDLE;
    }
    if (scenePipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, scenePipelineLayout_, nullptr);
        scenePipelineLayout_ = VK_NULL_HANDLE;
    }
    if (pipelineCache_ != VK_NULL_HANDLE) {
        if (persistPipelineCache) {
            try {
                savePipelineCache();
            } catch (const std::exception& exception) {
                try {
                    logger()->warn("Failed to save Vulkan pipeline cache during shutdown: {}", exception.what());
                } catch (...) {
                }
            } catch (...) {
                try {
                    logger()->warn("Failed to save Vulkan pipeline cache during shutdown");
                } catch (...) {
                }
            }
        }
        vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
        pipelineCache_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (tonemapSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, tonemapSetLayout_, nullptr);
        tonemapSetLayout_ = VK_NULL_HANDLE;
    }
    if (sceneSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, sceneSetLayout_, nullptr);
        sceneSetLayout_ = VK_NULL_HANDLE;
    }
    if (linearSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, linearSampler_, nullptr);
        linearSampler_ = VK_NULL_HANDLE;
    }
    if (textureSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, textureSampler_, nullptr);
        textureSampler_ = VK_NULL_HANDLE;
    }
    if (graphicsCommandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, graphicsCommandPool_, nullptr);
        graphicsCommandPool_ = VK_NULL_HANDLE;
    }
    if (transferCommandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, transferCommandPool_, nullptr);
        transferCommandPool_ = VK_NULL_HANDLE;
    }
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) {
            destroy(instance_, debugMessenger_, nullptr);
        }
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::requestScreenshot(std::filesystem::path path) {
    const std::scoped_lock lock{screenshotRequestMutex_};
    screenshotPath_ = std::move(path);
    screenshotPending_ = true;
}

void VulkanRenderer::waitIdle() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (timestampsEnabled_) {
            const std::uint32_t lastFrameIndex = static_cast<std::uint32_t>((frameIndex_ + kMaxFramesInFlight - 1U) % kMaxFramesInFlight);
            readBackGpuTimestamp(lastFrameIndex);
        }
    }
}

void VulkanRenderer::createInstance() {
    validationEnabled_ = config_.validation && validationLayerAvailable();
    deviceInfo_.validationEnabled = validationEnabled_;
    if (config_.validation && !validationEnabled_) {
        logger()->warn("Validation requested but VK_LAYER_KHRONOS_validation is not available");
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = config_.applicationName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "VolkEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    auto extensions = requiredInstanceExtensions();
    debugUtilsEnabled_ = instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    deviceInfo_.debugMarkers = debugUtilsEnabled_;
    if (debugUtilsEnabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerCreateInfo();
    if (validationEnabled_) {
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
        createInfo.pNext = &debugInfo;
    }

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");
}

bool VulkanRenderer::validationLayerAvailable() const {
    std::uint32_t layerCount = 0;
    checkVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "vkEnumerateInstanceLayerProperties count");
    std::vector<VkLayerProperties> layers(layerCount);
    checkVk(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "vkEnumerateInstanceLayerProperties data");

    for (const char* required : kValidationLayers) {
        const bool found = std::ranges::any_of(layers, [required](const VkLayerProperties& layer) {
            return std::strcmp(required, layer.layerName) == 0;
        });
        if (!found) { return false; }
    }
    return true;
}

std::vector<const char*> VulkanRenderer::requiredInstanceExtensions() const {
    std::uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions) {
        throw std::runtime_error("GLFW did not report required Vulkan instance extensions");
    }
    return {glfwExtensions, glfwExtensions + glfwExtensionCount};
}

bool VulkanRenderer::instanceExtensionAvailable(const char* extensionName) const {
    std::uint32_t extensionCount = 0;
    checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr), "vkEnumerateInstanceExtensionProperties count");
    std::vector<VkExtensionProperties> extensions(extensionCount);
    checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data()), "vkEnumerateInstanceExtensionProperties data");
    return std::ranges::any_of(extensions, [extensionName](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, extensionName) == 0;
    });
}

void VulkanRenderer::createDebugMessenger() {
    if (!validationEnabled_ || !debugUtilsEnabled_) { return; }
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (!create) {
        throw std::runtime_error("vkCreateDebugUtilsMessengerEXT is unavailable");
    }
    VkDebugUtilsMessengerCreateInfoEXT createInfo = debugMessengerCreateInfo();
    checkVk(create(instance_, &createInfo, nullptr, &debugMessenger_), "vkCreateDebugUtilsMessengerEXT");
}

void VulkanRenderer::loadDebugUtils() {
    if (!debugUtilsEnabled_) { return; }
    vkSetDebugUtilsObjectNameEXT_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
    vkCmdBeginDebugUtilsLabelEXT_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
    vkCmdEndDebugUtilsLabelEXT_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
    setObjectName(VK_OBJECT_TYPE_DEVICE, handleToUint64(device_), "VolkEngine Logical Device");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(graphicsQueue_), "Graphics Queue");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(presentQueue_), "Present Queue");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(transferQueue_), "Transfer Queue");
}

void VulkanRenderer::setObjectName(const VkObjectType objectType, const std::uint64_t objectHandle, const std::string_view name) const {
    if (!vkSetDebugUtilsObjectNameEXT_ || objectHandle == 0U || name.empty()) {
        return;
    }
    const std::string ownedName{name};
    VkDebugUtilsObjectNameInfoEXT nameInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    nameInfo.objectType = objectType;
    nameInfo.objectHandle = objectHandle;
    nameInfo.pObjectName = ownedName.c_str();
    const VkResult result = vkSetDebugUtilsObjectNameEXT_(device_, &nameInfo);
    if (result != VK_SUCCESS) {
        logger()->warn("vkSetDebugUtilsObjectNameEXT failed for {} with {}", ownedName, static_cast<int>(result));
    }
}

void VulkanRenderer::beginDebugLabel(const VkCommandBuffer commandBuffer, const char* name, const std::array<float, 4>& color) const {
    if (!vkCmdBeginDebugUtilsLabelEXT_ || name == nullptr || name[0] == '\0') {
        return;
    }
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    std::ranges::copy(color, label.color);
    vkCmdBeginDebugUtilsLabelEXT_(commandBuffer, &label);
}

void VulkanRenderer::endDebugLabel(const VkCommandBuffer commandBuffer) const {
    if (vkCmdEndDebugUtilsLabelEXT_) {
        vkCmdEndDebugUtilsLabelEXT_(commandBuffer);
    }
}

VulkanRenderer::DebugLabelScope::DebugLabelScope(const VulkanRenderer& renderer,
                                                 const VkCommandBuffer commandBuffer,
                                                 const char* name,
                                                 const std::array<float, 4>& color) noexcept
    : renderer_(&renderer), commandBuffer_(commandBuffer),
      active_(renderer.vkCmdBeginDebugUtilsLabelEXT_ != nullptr && name != nullptr && name[0] != '\0') {
    if (active_) {
        renderer_->beginDebugLabel(commandBuffer_, name, color);
    }
}

VulkanRenderer::DebugLabelScope::~DebugLabelScope() noexcept {
    if (active_ && renderer_ != nullptr) {
        renderer_->endDebugLabel(commandBuffer_);
    }
}

void VulkanRenderer::createSurface() {
    window_.createSurface(instance_, &surface_);
}

void VulkanRenderer::pickPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices count");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan-capable physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices data");

    std::multimap<int, VkPhysicalDevice, std::greater<>> ranked;
    std::vector<std::string> rejectionMessages;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        const DeviceSuitability suitability = evaluateDeviceSuitability(device);
        if (!suitability.suitable) {
            std::string message = properties.deviceName;
            message += ": ";
            for (std::size_t i = 0; i < suitability.reasons.size(); ++i) {
                if (i > 0U) { message += "; "; }
                message += suitability.reasons[i];
            }
            rejectionMessages.push_back(std::move(message));
            continue;
        }
        int score = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { score += 1000; }
        score += static_cast<int>(properties.limits.maxImageDimension2D);
        ranked.emplace(score, device);
    }

    if (ranked.empty()) {
        std::string message = "No suitable Vulkan physical device found";
        if (!rejectionMessages.empty()) {
            message += ":";
            for (const std::string& rejection : rejectionMessages) {
                message += "\n - ";
                message += rejection;
            }
        }
        throw std::runtime_error(message);
    }

    physicalDevice_ = ranked.begin()->second;
    queueFamilies_ = findQueueFamilies(physicalDevice_);
    vkGetPhysicalDeviceProperties(physicalDevice_, &physicalDeviceProperties_);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &physicalDeviceMemoryProperties_);
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features2);
    deviceInfo_.backend = RenderBackend::Vulkan;
    deviceInfo_.adapterName = physicalDeviceProperties_.deviceName;
    deviceInfo_.apiVersionMajor = VK_VERSION_MAJOR(physicalDeviceProperties_.apiVersion);
    deviceInfo_.apiVersionMinor = VK_VERSION_MINOR(physicalDeviceProperties_.apiVersion);
    deviceInfo_.apiVersionPatch = VK_VERSION_PATCH(physicalDeviceProperties_.apiVersion);
    deviceInfo_.maxImageDimension2D = physicalDeviceProperties_.limits.maxImageDimension2D;
    deviceInfo_.discreteGpu = physicalDeviceProperties_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    deviceInfo_.dynamicRendering = features13.dynamicRendering == VK_TRUE;
    deviceInfo_.synchronization2 = features13.synchronization2 == VK_TRUE;
    deviceInfo_.descriptorIndexing = features12.descriptorIndexing == VK_TRUE;
    deviceInfo_.bindlessSampledImagesSupported = supportsBindlessSampledImages(features12);
    deviceInfo_.multiDrawIndirect = features2.features.multiDrawIndirect == VK_TRUE;
    deviceInfo_.drawIndirectFirstInstance = features2.features.drawIndirectFirstInstance == VK_TRUE;
    deviceInfo_.samplerAnisotropy = features2.features.samplerAnisotropy == VK_TRUE;
    deviceInfo_.maxSamplerAnisotropy = deviceInfo_.samplerAnisotropy ? physicalDeviceProperties_.limits.maxSamplerAnisotropy : 1.0f;
    deviceInfo_.maxDrawIndirectCount = physicalDeviceProperties_.limits.maxDrawIndirectCount;
    logger()->info("Selected GPU: {} ({}, Vulkan {}.{}.{})",
                   physicalDeviceProperties_.deviceName,
                   gpuClassName(deviceInfo_.discreteGpu),
                   VK_VERSION_MAJOR(physicalDeviceProperties_.apiVersion),
                   VK_VERSION_MINOR(physicalDeviceProperties_.apiVersion),
                   VK_VERSION_PATCH(physicalDeviceProperties_.apiVersion));
}

bool VulkanRenderer::deviceExtensionAvailable(VkPhysicalDevice device, const char* extensionName) const {
    std::uint32_t extensionCount = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties count");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data()), "vkEnumerateDeviceExtensionProperties data");
    return std::ranges::any_of(available, [extensionName](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, extensionName) == 0;
    });
}

VulkanRenderer::DeviceSuitability VulkanRenderer::evaluateDeviceSuitability(VkPhysicalDevice device) const {
    DeviceSuitability result{};
    const auto reject = [&result](std::string reason) {
        result.suitable = false;
        result.reasons.push_back(std::move(reason));
    };

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        reject("Vulkan API version is below 1.3");
    }

    const QueueFamilies families = findQueueFamilies(device);
    if (!families.graphics.has_value()) { reject("missing graphics queue family"); }
    if (!families.present.has_value()) { reject("missing present queue family for the window surface"); }
    if (!families.transfer.has_value()) { reject("missing transfer queue family"); }

    std::uint32_t extensionCount = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties count");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data()), "vkEnumerateDeviceExtensionProperties data");
    std::set<std::string_view> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const VkExtensionProperties& extension : available) {
        required.erase(extension.extensionName);
    }
    for (const std::string_view missing : required) {
        reject("missing device extension " + std::string(missing));
    }

    if (required.empty()) {
        const SwapchainSupport swapchainSupport = querySwapchainSupport(device);
        if (swapchainSupport.formats.empty()) { reject("window surface exposes no swapchain formats"); }
        if (swapchainSupport.presentModes.empty()) { reject("window surface exposes no present modes"); }
        if ((swapchainSupport.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0U) {
            reject("window surface swapchain images cannot be color attachments");
        }
    }

    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(device, &features2);
    if (features13.dynamicRendering != VK_TRUE) { reject("missing Vulkan 1.3 dynamicRendering feature"); }
    if (features13.synchronization2 != VK_TRUE) { reject("missing Vulkan 1.3 synchronization2 feature"); }

    return result;
}

VulkanRenderer::QueueFamilies VulkanRenderer::findQueueFamilies(VkPhysicalDevice device) const {
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, families.data());

    QueueFamilies result{};
    for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
        const VkQueueFamilyProperties& family = families[i];
        if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
            result.graphics = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport), "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (presentSupport == VK_TRUE) {
            result.present = i;
        }

        if ((family.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0U) {
            if (!result.transfer.has_value() || ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U)) {
                result.transfer = i;
            }
        }
    }
    return result;
}

VulkanRenderer::SwapchainSupport VulkanRenderer::querySwapchainSupport(VkPhysicalDevice device) const {
    SwapchainSupport support{};
    checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &support.capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    std::uint32_t formatCount = 0;
    checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR count");
    support.formats.resize(formatCount);
    if (formatCount > 0U) {
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, support.formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR data");
    }

    std::uint32_t presentModeCount = 0;
    checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR count");
    support.presentModes.resize(presentModeCount);
    if (presentModeCount > 0U) {
        checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, support.presentModes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR data");
    }
    return support;
}

void VulkanRenderer::createLogicalDevice() {
    std::set<std::uint32_t> uniqueFamilies{queueFamilies_.graphics.value(), queueFamilies_.present.value(), queueFamilies_.transfer.value()};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const float queuePriority = 1.0f;
    for (std::uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 supportedFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures2);
    VkPhysicalDeviceFeatures deviceFeatures{};
    const float deviceMaxSamplerAnisotropy = std::max(1.0f, physicalDeviceProperties_.limits.maxSamplerAnisotropy);
    samplerAnisotropyEnabled_ = supportedFeatures2.features.samplerAnisotropy == VK_TRUE && deviceMaxSamplerAnisotropy > 1.0f;
    maxSamplerAnisotropy_ = samplerAnisotropyEnabled_ ? std::min(deviceMaxSamplerAnisotropy, 16.0f) : 1.0f;
    if (samplerAnisotropyEnabled_) {
        deviceFeatures.samplerAnisotropy = VK_TRUE;
    }
    multiDrawIndirectEnabled_ = supportedFeatures2.features.multiDrawIndirect == VK_TRUE;
    drawIndirectFirstInstanceEnabled_ = supportedFeatures2.features.drawIndirectFirstInstance == VK_TRUE;
    if (multiDrawIndirectEnabled_) {
        deviceFeatures.multiDrawIndirect = VK_TRUE;
    }
    if (drawIndirectFirstInstanceEnabled_) {
        deviceFeatures.drawIndirectFirstInstance = VK_TRUE;
    }
    indirectSceneDrawsEnabled_ = config_.indirectSceneDraws &&
                                  multiDrawIndirectEnabled_ && drawIndirectFirstInstanceEnabled_ &&
                                  physicalDeviceProperties_.limits.maxDrawIndirectCount >= kSceneMeshBatchOrder.size();
    if (config_.indirectSceneDraws && !indirectSceneDrawsEnabled_) {
        std::string reason;
        const auto appendReason = [&reason](const std::string_view text) {
            if (!reason.empty()) {
                reason += ", ";
            }
            reason += text;
        };
        if (!multiDrawIndirectEnabled_) {
            appendReason("multiDrawIndirect unsupported");
        }
        if (!drawIndirectFirstInstanceEnabled_) {
            appendReason("drawIndirectFirstInstance unsupported");
        }
        if (physicalDeviceProperties_.limits.maxDrawIndirectCount < kSceneMeshBatchOrder.size()) {
            appendReason("maxDrawIndirectCount " + std::to_string(physicalDeviceProperties_.limits.maxDrawIndirectCount)
                         + " < required " + std::to_string(kSceneMeshBatchOrder.size()));
        }
        logger()->warn("Indirect scene draws requested but disabled: {}; using direct indexed-instanced fallback", reason);
    }
    deviceInfo_.multiDrawIndirect = multiDrawIndirectEnabled_;
    deviceInfo_.drawIndirectFirstInstance = drawIndirectFirstInstanceEnabled_;
    deviceInfo_.indirectSceneDraws = indirectSceneDrawsEnabled_;
    deviceInfo_.samplerAnisotropy = samplerAnisotropyEnabled_;
    deviceInfo_.maxSamplerAnisotropy = maxSamplerAnisotropy_;

    std::vector<const char*> enabledExtensions(kDeviceExtensions.begin(), kDeviceExtensions.end());
    if (deviceExtensionAvailable(physicalDevice_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        memoryBudgetEnabled_ = true;
        deviceInfo_.memoryBudget = true;
    }

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &features13;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamilies_.graphics.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.present.value(), 0, &presentQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.transfer.value(), 0, &transferQueue_);
    deviceInfo_.transferUploadSync = transferQueue_ != graphicsQueue_ ? TransferUploadSyncMode::QueueSemaphore : TransferUploadSyncMode::SameQueueBarrier;
}

void VulkanRenderer::createAllocator() {
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = physicalDevice_;
    allocatorInfo.device = device_;
    allocatorInfo.instance = instance_;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    if (memoryBudgetEnabled_) {
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }

    checkVk(vmaCreateAllocator(&allocatorInfo, &allocator_), "vmaCreateAllocator");
}

void VulkanRenderer::createCommandPools() {
    VkCommandPoolCreateInfo graphicsInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    graphicsInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    graphicsInfo.queueFamilyIndex = queueFamilies_.graphics.value();
    checkVk(vkCreateCommandPool(device_, &graphicsInfo, nullptr, &graphicsCommandPool_), "vkCreateCommandPool graphics");
    setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(graphicsCommandPool_), "Graphics Command Pool");

    VkCommandPoolCreateInfo transferInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    transferInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    transferInfo.queueFamilyIndex = queueFamilies_.transfer.value();
    checkVk(vkCreateCommandPool(device_, &transferInfo, nullptr, &transferCommandPool_), "vkCreateCommandPool transfer");
    setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(transferCommandPool_), "Transfer Command Pool");
}

VkSurfaceFormatKHR VulkanRenderer::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    // The tonemap shader writes gamma-encoded LDR values. Prefer UNORM swapchain
    // formats so Vulkan does not apply a second sRGB conversion on color writes.
    for (const VkSurfaceFormatKHR& format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR VulkanRenderer::choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const {
    if (!config_.vsync) {
        for (const VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) { return mode; }
        }
        for (const VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) { return mode; }
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent = window_.framebufferExtent();
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

void VulkanRenderer::createSwapchain() {
    const SwapchainSupport support = querySwapchainSupport(physicalDevice_);
    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    presentMode_ = choosePresentMode(support.presentModes);
    const VkExtent2D extent = chooseExtent(support.capabilities);

    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = surface_;
    createInfo.minImageCount = clampImageCount(support.capabilities);
    swapchainMinImageCount_ = createInfo.minImageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0U) {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    } else {
        logger()->warn("Swapchain images do not support TRANSFER_DST usage; continuing with color-attachment usage only");
    }
    swapchainTransferSrcSupported_ = (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U;
    if (swapchainTransferSrcSupported_) {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    const std::array<std::uint32_t, 2> queueFamilyIndices{queueFamilies_.graphics.value(), queueFamilies_.present.value()};
    if (queueFamilies_.graphics != queueFamilies_.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilyIndices.size());
        createInfo.pQueueFamilyIndices = queueFamilyIndices.data();
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode_;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    checkVk(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    setObjectName(VK_OBJECT_TYPE_SWAPCHAIN_KHR, handleToUint64(swapchain_), "Main Swapchain");
    std::uint32_t imageCount = 0;
    checkVk(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr), "vkGetSwapchainImagesKHR count");
    swapchainImages_.resize(imageCount);
    checkVk(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data()), "vkGetSwapchainImagesKHR data");
    swapchainFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
    swapchainStates_.assign(imageCount, {});


    swapchainRenderFinishedSemaphores_.resize(imageCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (std::size_t i = 0; i < swapchainRenderFinishedSemaphores_.size(); ++i) {
        checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &swapchainRenderFinishedSemaphores_[i]), "vkCreateSemaphore swapchain renderFinished");
        setObjectName(VK_OBJECT_TYPE_SEMAPHORE, handleToUint64(swapchainRenderFinishedSemaphores_[i]),
                      "Swapchain Image " + std::to_string(i) + " Render Finished Semaphore");
    }
    logger()->info("Created swapchain {}x{} with {} images ({})", extent.width, extent.height, imageCount, presentModeName(presentMode_));
}

void VulkanRenderer::createImageViews() {
    swapchainImageViews_.resize(swapchainImages_.size());
    swapchainResourceIds_.resize(swapchainImages_.size(), GpuResourceRegistry::kInvalidId);
    for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
        swapchainImageViews_[i] = createImageView(swapchainImages_[i], swapchainFormat_, VK_IMAGE_ASPECT_COLOR_BIT);
        const std::string index = std::to_string(i);
        setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(swapchainImages_[i]), "Swapchain Image " + index);
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(swapchainImageViews_[i]), "Swapchain Image View " + index);
        const std::uint64_t swapchainBytes = imageByteEstimate(swapchainExtent_, swapchainFormat_);
        swapchainResourceIds_[i] = resourceRegistry_.registerResource(GpuResourceKind::Image, "Swapchain Image", swapchainBytes, true);
    }
}

VkImageView VulkanRenderer::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, const std::uint32_t mipLevels) const {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &view), "vkCreateImageView");
    return view;
}

VkFormat VulkanRenderer::findDepthFormat() const {
    constexpr std::array<VkFormat, 3> candidates{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
            return format;
        }
    }
    throw std::runtime_error("No supported depth format found");
}

void VulkanRenderer::createDepthResources() {
    depth_ = createImage(swapchainExtent_, findDepthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(depth_.image), "Depth Image");
    vmaSetAllocationName(allocator_, depth_.allocation, "Depth Image Allocation");
    setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(depth_.view), "Depth Image View");
    const std::uint64_t depthBytes = imageByteEstimate(depth_.extent, depth_.format);
    depth_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Image, "Depth Image", depthBytes);
}

void VulkanRenderer::createHdrResources() {
    hdr_ = createImage(swapchainExtent_, VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(hdr_.image), "HDR Color Image");
    vmaSetAllocationName(allocator_, hdr_.allocation, "HDR Color Image Allocation");
    setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(hdr_.view), "HDR Color Image View");
    const std::uint64_t hdrBytes = imageByteEstimate(hdr_.extent, hdr_.format);
    hdr_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Image, "HDR Color Image", hdrBytes);
}

VulkanRenderer::ImageResource VulkanRenderer::createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, const std::uint32_t mipLevels) {
    ImageResource resource{};
    resource.format = format;
    resource.extent = extent;
    resource.mipLevels = mipLevels;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    checkVk(vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &resource.image, &resource.allocation, nullptr), "vmaCreateImage");

    resource.view = createImageView(resource.image, format, aspectFlags, mipLevels);
    return resource;
}

void VulkanRenderer::destroyImage(ImageResource& image) {
    resourceRegistry_.unregisterResource(image.resourceId);
    image.resourceId = GpuResourceRegistry::kInvalidId;
    if (image.view != VK_NULL_HANDLE) { vkDestroyImageView(device_, image.view, nullptr); }
    if (image.image != VK_NULL_HANDLE) { vmaDestroyImage(allocator_, image.image, image.allocation); }
    image = {};
}

void VulkanRenderer::createTextureResources() {
    const std::filesystem::path texturePath = config_.assetDirectory / "textures" / "ground_albedo.ppm";
    const LoadedImageRgba8 texture = loadPpmRgba8(texturePath);
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(texture.pixels.size());

    Buffer staging{};
    std::vector<Buffer> stagingBuffers;
    stagingBuffers.reserve(1);
    try {
        staging = createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap stagingMap{allocator_, staging.allocation, "vmaMapMemory texture staging"};
            std::memcpy(stagingMap.get(), texture.pixels.data(), texture.pixels.size());
        }

        const VkExtent2D textureExtent{texture.width, texture.height};
        const VkFormat textureFormat = VK_FORMAT_R8G8B8A8_SRGB;
        const std::uint32_t requestedMipLevels = mipLevelCountForExtent(textureExtent);
        const bool canGenerateMipmaps = requestedMipLevels > 1U && formatSupportsLinearMipBlit(textureFormat);
        const std::uint32_t mipLevels = canGenerateMipmaps ? requestedMipLevels : 1U;
        if (requestedMipLevels > 1U && !canGenerateMipmaps) {
            logger()->warn("Texture format VK_FORMAT_R8G8B8A8_SRGB lacks linear blit/filter support; loading {} with one mip", texturePath.string());
        }
        VkImageUsageFlags textureUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (mipLevels > 1U) {
            textureUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        groundAlbedoTexture_ = createImage(textureExtent, textureFormat, textureUsage, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
        setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(groundAlbedoTexture_.image), "Ground Albedo Texture");
        vmaSetAllocationName(allocator_, groundAlbedoTexture_.allocation, "Ground Albedo Texture Allocation");
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(groundAlbedoTexture_.view), "Ground Albedo Texture View");
        groundAlbedoTexture_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Image, "Ground Albedo Texture",
                                                                             imageByteEstimate(groundAlbedoTexture_.extent, groundAlbedoTexture_.format, groundAlbedoTexture_.mipLevels));

        VkCommandBuffer uploadCommands = beginGraphicsUploadCommands();
        transitionImageTracked(uploadCommands, groundAlbedoTexture_.image, groundAlbedoTexture_.syncState,
                               ImageSyncState{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                               VK_IMAGE_ASPECT_COLOR_BIT, 0, groundAlbedoTexture_.mipLevels);
        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {texture.width, texture.height, 1};
        vkCmdCopyBufferToImage(uploadCommands, staging.buffer, groundAlbedoTexture_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        if (groundAlbedoTexture_.mipLevels > 1U) {
            generateMipmaps(uploadCommands, groundAlbedoTexture_);
        } else {
            transitionImageTracked(uploadCommands, groundAlbedoTexture_.image, groundAlbedoTexture_.syncState,
                                   imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::SampledImage),
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        }
        stagingBuffers.push_back(staging);
        staging = {};
        submitGraphicsUpload(uploadCommands, std::move(stagingBuffers));
        logger()->info("Loaded texture {} ({}x{} RGBA8, {} mips)", texturePath.string(), texture.width, texture.height, groundAlbedoTexture_.mipLevels);
    } catch (...) {
        destroyBuffer(staging);
        destroyImage(groundAlbedoTexture_);
        throw;
    }
}



void VulkanRenderer::createSampler() {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &linearSampler_), "vkCreateSampler");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(linearSampler_), "Linear Clamp Sampler");

    VkSamplerCreateInfo textureSamplerInfo = samplerInfo;
    textureSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.maxLod = groundAlbedoTexture_.mipLevels > 0U ? static_cast<float>(groundAlbedoTexture_.mipLevels - 1U) : 0.0f;
    textureSamplerInfo.anisotropyEnable = samplerAnisotropyEnabled_ ? VK_TRUE : VK_FALSE;
    textureSamplerInfo.maxAnisotropy = maxSamplerAnisotropy_;
    checkVk(vkCreateSampler(device_, &textureSamplerInfo, nullptr, &textureSampler_), "vkCreateSampler texture");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(textureSampler_), "Linear Repeat Texture Sampler");
}

void VulkanRenderer::createDescriptorLayouts() {
    std::array<VkDescriptorSetLayoutBinding, 3> sceneBindings{};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[1].binding = 1;
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneBindings[1].descriptorCount = 1;
    sceneBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[2].binding = 2;
    sceneBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[2].descriptorCount = 1;
    sceneBindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo sceneInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sceneInfo.bindingCount = static_cast<std::uint32_t>(sceneBindings.size());
    sceneInfo.pBindings = sceneBindings.data();
    checkVk(vkCreateDescriptorSetLayout(device_, &sceneInfo, nullptr, &sceneSetLayout_), "vkCreateDescriptorSetLayout scene");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, handleToUint64(sceneSetLayout_), "Scene Descriptor Set Layout");

    VkDescriptorSetLayoutBinding tonemapBinding{};
    tonemapBinding.binding = 0;
    tonemapBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tonemapBinding.descriptorCount = 1;
    tonemapBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo tonemapInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tonemapInfo.bindingCount = 1;
    tonemapInfo.pBindings = &tonemapBinding;
    checkVk(vkCreateDescriptorSetLayout(device_, &tonemapInfo, nullptr, &tonemapSetLayout_), "vkCreateDescriptorSetLayout tonemap");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, handleToUint64(tonemapSetLayout_), "Tonemap Descriptor Set Layout");

    // Renderer-owned descriptor sets are allocated once at startup; the ImGui backend owns a
    // separate pool so scene/tonemap descriptor pressure stays fixed and free of frame-loop churn.
    constexpr std::uint32_t sceneSetCount = kMaxFramesInFlight;
    constexpr std::uint32_t tonemapSetCount = 1U;
    constexpr std::uint32_t rendererSetCount = sceneSetCount + tonemapSetCount;
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sceneSetCount};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sceneSetCount + tonemapSetCount};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sceneSetCount};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = rendererSetCount;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, handleToUint64(descriptorPool_), "Renderer Descriptor Pool");
}

std::filesystem::path VulkanRenderer::pipelineCachePath() const {
    return config_.cacheDirectory / "pipeline_cache.bin";
}

bool VulkanRenderer::pipelineCacheDataMatchesDevice(const std::vector<std::byte>& data) const {
    if (data.size() < sizeof(PipelineCacheHeader)) {
        return false;
    }

    PipelineCacheHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(PipelineCacheHeader) || static_cast<std::size_t>(header.headerSize) > data.size()) {
        return false;
    }
    if (header.headerVersion != static_cast<std::uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) {
        return false;
    }
    if (header.vendorID != physicalDeviceProperties_.vendorID || header.deviceID != physicalDeviceProperties_.deviceID) {
        return false;
    }
    return std::equal(header.pipelineCacheUUID.begin(), header.pipelineCacheUUID.end(), physicalDeviceProperties_.pipelineCacheUUID);
}

std::vector<std::byte> VulkanRenderer::loadPipelineCacheData() const {
    const std::filesystem::path path = pipelineCachePath();
    if (!std::filesystem::exists(path)) {
        return {};
    }

    try {
        std::vector<std::byte> data = readBinaryFile(path);
        if (pipelineCacheDataMatchesDevice(data)) {
            logger()->info("Loaded Vulkan pipeline cache: {} bytes from {}", data.size(), path.string());
            return data;
        }
        logger()->warn("Ignoring incompatible Vulkan pipeline cache at {}", path.string());
    } catch (const std::exception& e) {
        logger()->warn("Failed to load Vulkan pipeline cache {}: {}", path.string(), e.what());
    }
    return {};
}

void VulkanRenderer::createPipelineCache() {
    const std::vector<std::byte> initialData = loadPipelineCacheData();
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = initialData.size();
    cacheInfo.pInitialData = initialData.empty() ? nullptr : initialData.data();
    checkVk(vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_), "vkCreatePipelineCache");
    setObjectName(VK_OBJECT_TYPE_PIPELINE_CACHE, handleToUint64(pipelineCache_), "Renderer Pipeline Cache");
}

void VulkanRenderer::savePipelineCache() const {
    std::size_t dataSize = 0;
    VkResult result = vkGetPipelineCacheData(device_, pipelineCache_, &dataSize, nullptr);
    if (result != VK_SUCCESS || dataSize == 0U) {
        logger()->warn("Skipping Vulkan pipeline cache save; size query returned {}", static_cast<int>(result));
        return;
    }

    std::vector<std::byte> data;
    for (std::uint32_t attempt = 0; attempt < 3U; ++attempt) {
        data.assign(dataSize, std::byte{});
        std::size_t writableSize = data.size();
        result = vkGetPipelineCacheData(device_, pipelineCache_, &writableSize, data.data());
        if (result == VK_SUCCESS) {
            data.resize(writableSize);
            break;
        }
        if (result != VK_INCOMPLETE) {
            logger()->warn("Skipping Vulkan pipeline cache save; read returned {}", static_cast<int>(result));
            return;
        }

        result = vkGetPipelineCacheData(device_, pipelineCache_, &dataSize, nullptr);
        if (result != VK_SUCCESS || dataSize == 0U) {
            logger()->warn("Skipping Vulkan pipeline cache save; retry size query returned {}", static_cast<int>(result));
            return;
        }
    }
    if (result != VK_SUCCESS) {
        logger()->warn("Skipping Vulkan pipeline cache save; cache kept growing during readback");
        return;
    }

    if (!pipelineCacheDataMatchesDevice(data)) {
        logger()->warn("Skipping Vulkan pipeline cache save; generated cache header does not match selected device");
        return;
    }

    const std::filesystem::path path = pipelineCachePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        logger()->warn("Failed to create pipeline cache directory {}: {}", path.parent_path().string(), error.message());
        return;
    }

    const std::filesystem::path temporaryPath = uniquePipelineCacheTemporaryPath(path);
    {
        std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!file) {
            logger()->warn("Failed to open temporary Vulkan pipeline cache for writing: {}", temporaryPath.string());
            return;
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        file.close();
        if (!file) {
            logger()->warn("Failed to write temporary Vulkan pipeline cache: {}", temporaryPath.string());
            std::filesystem::remove(temporaryPath, error);
            return;
        }
    }

    error.clear();
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        error.clear();
        std::filesystem::rename(temporaryPath, path, error);
    }
    if (error) {
        logger()->warn("Failed to publish Vulkan pipeline cache {}; discarded temporary cache {}: {}",
                       path.string(), temporaryPath.string(), error.message());
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return;
    }
    logger()->info("Saved Vulkan pipeline cache: {} bytes to {}", data.size(), path.string());
}

VulkanRenderer::PipelineSet VulkanRenderer::buildPipelineSet() {
    PipelineSet pipelines{};
    const std::array<std::filesystem::path, 4> shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    VkShaderModule sceneVert = VK_NULL_HANDLE;
    VkShaderModule sceneFrag = VK_NULL_HANDLE;
    VkShaderModule tonemapVert = VK_NULL_HANDLE;
    VkShaderModule tonemapFrag = VK_NULL_HANDLE;

    try {
        sceneVert = createShaderModule(shaderPaths[0]);
        sceneFrag = createShaderModule(shaderPaths[1]);
        tonemapVert = createShaderModule(shaderPaths[2]);
        tonemapFrag = createShaderModule(shaderPaths[3]);

        std::array<VkPipelineShaderStageCreateInfo, 2> sceneStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, sceneVert), shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, sceneFrag)};
        std::array<VkVertexInputBindingDescription, 1> bindings{};
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(Vertex);
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 3> attributes{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)};

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthPrepassDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthPrepassDepth.depthTestEnable = VK_TRUE;
        depthPrepassDepth.depthWriteEnable = VK_TRUE;
        depthPrepassDepth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineDepthStencilStateCreateInfo sceneDepth = depthPrepassDepth;
        sceneDepth.depthWriteEnable = VK_FALSE;
        sceneDepth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkPipelineColorBlendStateCreateInfo noColorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        noColorBlend.attachmentCount = 0;

        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        VkPipelineLayoutCreateInfo sceneLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        sceneLayoutInfo.setLayoutCount = 1;
        sceneLayoutInfo.pSetLayouts = &sceneSetLayout_;
        checkVk(vkCreatePipelineLayout(device_, &sceneLayoutInfo, nullptr, &pipelines.sceneLayout), "vkCreatePipelineLayout scene");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, handleToUint64(pipelines.sceneLayout), "Scene Pipeline Layout");

        VkPipelineRenderingCreateInfo sceneRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        sceneRendering.colorAttachmentCount = 1;
        sceneRendering.pColorAttachmentFormats = &hdr_.format;
        sceneRendering.depthAttachmentFormat = depth_.format;

        std::array<VkPipelineShaderStageCreateInfo, 1> depthPrepassStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, sceneVert)};
        VkPipelineRenderingCreateInfo depthPrepassRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        depthPrepassRendering.colorAttachmentCount = 0;
        depthPrepassRendering.depthAttachmentFormat = depth_.format;
        VkGraphicsPipelineCreateInfo depthPrepassInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        depthPrepassInfo.pNext = &depthPrepassRendering;
        depthPrepassInfo.stageCount = static_cast<std::uint32_t>(depthPrepassStages.size());
        depthPrepassInfo.pStages = depthPrepassStages.data();
        depthPrepassInfo.pVertexInputState = &vertexInput;
        depthPrepassInfo.pInputAssemblyState = &inputAssembly;
        depthPrepassInfo.pViewportState = &viewportState;
        depthPrepassInfo.pRasterizationState = &rasterizer;
        depthPrepassInfo.pMultisampleState = &multisample;
        depthPrepassInfo.pDepthStencilState = &depthPrepassDepth;
        depthPrepassInfo.pColorBlendState = &noColorBlend;
        depthPrepassInfo.pDynamicState = &dynamic;
        depthPrepassInfo.layout = pipelines.sceneLayout;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &depthPrepassInfo, nullptr, &pipelines.depthPrepass), "vkCreateGraphicsPipelines depth prepass");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.depthPrepass), "Depth Prepass Pipeline");

        VkGraphicsPipelineCreateInfo sceneInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        sceneInfo.pNext = &sceneRendering;
        sceneInfo.stageCount = static_cast<std::uint32_t>(sceneStages.size());
        sceneInfo.pStages = sceneStages.data();
        sceneInfo.pVertexInputState = &vertexInput;
        sceneInfo.pInputAssemblyState = &inputAssembly;
        sceneInfo.pViewportState = &viewportState;
        sceneInfo.pRasterizationState = &rasterizer;
        sceneInfo.pMultisampleState = &multisample;
        sceneInfo.pDepthStencilState = &sceneDepth;
        sceneInfo.pColorBlendState = &blend;
        sceneInfo.pDynamicState = &dynamic;
        sceneInfo.layout = pipelines.sceneLayout;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &sceneInfo, nullptr, &pipelines.scene), "vkCreateGraphicsPipelines scene");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.scene), "HDR Scene Pipeline");

        sceneInfo.pDepthStencilState = &depthPrepassDepth;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &sceneInfo, nullptr, &pipelines.sceneNoPrepass), "vkCreateGraphicsPipelines scene no prepass");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.sceneNoPrepass), "HDR Scene Pipeline No Prepass");

        std::array<VkPipelineShaderStageCreateInfo, 2> tonemapStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, tonemapVert), shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, tonemapFrag)};
        VkPipelineVertexInputStateCreateInfo emptyVertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo noDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo noCullRasterizer = rasterizer;
        noCullRasterizer.cullMode = VK_CULL_MODE_NONE;

        VkPushConstantRange tonemapPushRange{};
        tonemapPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        tonemapPushRange.offset = 0;
        tonemapPushRange.size = sizeof(float);

        VkPipelineLayoutCreateInfo tonemapLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        tonemapLayoutInfo.setLayoutCount = 1;
        tonemapLayoutInfo.pSetLayouts = &tonemapSetLayout_;
        tonemapLayoutInfo.pushConstantRangeCount = 1;
        tonemapLayoutInfo.pPushConstantRanges = &tonemapPushRange;
        checkVk(vkCreatePipelineLayout(device_, &tonemapLayoutInfo, nullptr, &pipelines.tonemapLayout), "vkCreatePipelineLayout tonemap");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, handleToUint64(pipelines.tonemapLayout), "Tonemap Pipeline Layout");

        VkPipelineRenderingCreateInfo tonemapRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        tonemapRendering.colorAttachmentCount = 1;
        tonemapRendering.pColorAttachmentFormats = &swapchainFormat_;

        VkGraphicsPipelineCreateInfo tonemapInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        tonemapInfo.pNext = &tonemapRendering;
        tonemapInfo.stageCount = static_cast<std::uint32_t>(tonemapStages.size());
        tonemapInfo.pStages = tonemapStages.data();
        tonemapInfo.pVertexInputState = &emptyVertexInput;
        tonemapInfo.pInputAssemblyState = &inputAssembly;
        tonemapInfo.pViewportState = &viewportState;
        tonemapInfo.pRasterizationState = &noCullRasterizer;
        tonemapInfo.pMultisampleState = &multisample;
        tonemapInfo.pDepthStencilState = &noDepth;
        tonemapInfo.pColorBlendState = &blend;
        tonemapInfo.pDynamicState = &dynamic;
        tonemapInfo.layout = pipelines.tonemapLayout;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &tonemapInfo, nullptr, &pipelines.tonemap), "vkCreateGraphicsPipelines tonemap");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.tonemap), "Tonemap Pipeline");
    } catch (...) {
        if (tonemapFrag != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, tonemapFrag, nullptr); }
        if (tonemapVert != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, tonemapVert, nullptr); }
        if (sceneFrag != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, sceneFrag, nullptr); }
        if (sceneVert != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, sceneVert, nullptr); }
        destroyPipelineSet(pipelines);
        throw;
    }

    vkDestroyShaderModule(device_, tonemapFrag, nullptr);
    vkDestroyShaderModule(device_, tonemapVert, nullptr);
    vkDestroyShaderModule(device_, sceneFrag, nullptr);
    vkDestroyShaderModule(device_, sceneVert, nullptr);
    return pipelines;
}

void VulkanRenderer::createPipelines() {
    PipelineSet pipelines = buildPipelineSet();
    installPipelineSet(pipelines);
    refreshShaderWriteTimes();
}

void VulkanRenderer::destroyPipelineSet(PipelineSet& pipelines) const {
    if (pipelines.tonemap != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.tonemap, nullptr); pipelines.tonemap = VK_NULL_HANDLE; }
    if (pipelines.tonemapLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, pipelines.tonemapLayout, nullptr); pipelines.tonemapLayout = VK_NULL_HANDLE; }
    if (pipelines.depthPrepass != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.depthPrepass, nullptr); pipelines.depthPrepass = VK_NULL_HANDLE; }
    if (pipelines.scene != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.scene, nullptr); pipelines.scene = VK_NULL_HANDLE; }
    if (pipelines.sceneNoPrepass != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.sceneNoPrepass, nullptr); pipelines.sceneNoPrepass = VK_NULL_HANDLE; }
    if (pipelines.sceneLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, pipelines.sceneLayout, nullptr); pipelines.sceneLayout = VK_NULL_HANDLE; }
}

void VulkanRenderer::retireDeferredPipelineSets() {
    for (auto it = retiredPipelineSets_.begin(); it != retiredPipelineSets_.end();) {
        bool ready = true;
        for (const VkFence fence : it->completionFences) {
            if (fence == VK_NULL_HANDLE) {
                continue;
            }
            const VkResult status = vkGetFenceStatus(device_, fence);
            if (status == VK_NOT_READY) {
                ready = false;
                break;
            }
            checkVk(status, "vkGetFenceStatus retired pipeline set");
        }
        if (ready) {
            destroyPipelineSet(it->pipelines);
            it = retiredPipelineSets_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanRenderer::installPipelineSet(const PipelineSet& pipelines) {
    scenePipelineLayout_ = pipelines.sceneLayout;
    depthPrepassPipeline_ = pipelines.depthPrepass;
    scenePipeline_ = pipelines.scene;
    sceneNoPrepassPipeline_ = pipelines.sceneNoPrepass;
    tonemapPipelineLayout_ = pipelines.tonemapLayout;
    tonemapPipeline_ = pipelines.tonemap;
}

void VulkanRenderer::refreshShaderWriteTimes() {
    const std::array<std::filesystem::path, 4> shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    for (std::size_t i = 0; i < shaderPaths.size(); ++i) {
        std::error_code error;
        shaderWriteTimes_[i] = std::filesystem::last_write_time(shaderPaths[i], error);
        if (error) {
            shaderWriteTimes_[i] = {};
        }
    }
}

bool VulkanRenderer::shaderFilesChanged() const {
    const std::array<std::filesystem::path, 4> shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    for (std::size_t i = 0; i < shaderPaths.size(); ++i) {
        std::error_code error;
        const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(shaderPaths[i], error);
        if (error) {
            return false;
        }
        if (writeTime != shaderWriteTimes_[i]) {
            return true;
        }
    }
    return false;
}

void VulkanRenderer::pollShaderHotReload(const double elapsedSeconds) {
    if (!config_.shaderHotReload || elapsedSeconds - shaderHotReloadLastCheckSeconds_ < 0.5) {
        return;
    }
    shaderHotReloadLastCheckSeconds_ = elapsedSeconds;
    if (!shaderFilesChanged()) {
        return;
    }

    logger()->info("Detected shader bytecode change; rebuilding graphics pipelines");
    PipelineSet nextPipelines{};
    try {
        nextPipelines = buildPipelineSet();
    } catch (const std::exception& e) {
        logger()->warn("Shader hot reload failed; keeping existing pipelines: {}", e.what());
        refreshShaderWriteTimes();
        return;
    }

    PipelineSet oldPipelines{};
    oldPipelines.sceneLayout = scenePipelineLayout_;
    oldPipelines.depthPrepass = depthPrepassPipeline_;
    oldPipelines.scene = scenePipeline_;
    oldPipelines.sceneNoPrepass = sceneNoPrepassPipeline_;
    oldPipelines.tonemapLayout = tonemapPipelineLayout_;
    oldPipelines.tonemap = tonemapPipeline_;

    RetiredPipelineSet retired{};
    retired.pipelines = oldPipelines;
    for (std::size_t index = 0; index < frames_.size(); ++index) {
        retired.completionFences[index] = frames_[index].submittedOnce ? frames_[index].inFlight : VK_NULL_HANDLE;
    }
    try {
        retiredPipelineSets_.push_back(retired);
    } catch (...) {
        destroyPipelineSet(nextPipelines);
        throw;
    }
    installPipelineSet(nextPipelines);
    refreshShaderWriteTimes();
    logger()->info("Reloaded graphics pipelines from updated shader bytecode; retiring previous set after tracked frame fences signal");
}

VkShaderModule VulkanRenderer::createShaderModule(const std::filesystem::path& path) const {
    const std::vector<std::byte> bytes = readBinaryFile(path);
    constexpr std::uint32_t kSpirvMagic = 0x07230203U;
    constexpr std::uint32_t kByteSwappedSpirvMagic = 0x03022307U;
    constexpr std::size_t kSpirvHeaderWordCount = 5;
    if (bytes.size() < kSpirvHeaderWordCount * sizeof(std::uint32_t)) {
        throw std::runtime_error("SPIR-V file is too small to contain a valid header: " + path.string());
    }
    if (bytes.size() % sizeof(std::uint32_t) != 0U) {
        throw std::runtime_error("SPIR-V file has invalid byte size: " + path.string());
    }

    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (words[0] == kByteSwappedSpirvMagic) {
        throw std::runtime_error("SPIR-V file appears byte-swapped instead of native little-endian words: " + path.string());
    }
    if (words[0] != kSpirvMagic) {
        throw std::runtime_error("SPIR-V file has invalid magic number: " + path.string());
    }

    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = bytes.size();
    createInfo.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const std::string operation = "vkCreateShaderModule " + path.string();
    checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &module), operation.c_str());
    return module;
}

VkDeviceSize VulkanRenderer::checkedSceneInstanceBufferSize(std::size_t capacity) const {
    if (capacity == 0U) {
        capacity = 1U;
    }
    if (capacity > static_cast<std::size_t>(std::numeric_limits<VkDeviceSize>::max() / sizeof(InstanceData))) {
        throw std::runtime_error("Scene instance capacity exceeds Vulkan buffer size range");
    }

    const VkDeviceSize instanceBufferSize = static_cast<VkDeviceSize>(sizeof(InstanceData) * capacity);
    if (instanceBufferSize > physicalDeviceProperties_.limits.maxStorageBufferRange) {
        throw std::runtime_error("Scene instance storage exceeds VkPhysicalDeviceLimits::maxStorageBufferRange");
    }
    return instanceBufferSize;
}


void VulkanRenderer::createFrameInstanceDataBuffer(FrameResources& frame, const std::size_t frameIndex, std::size_t capacity) {
    if (capacity == 0U) {
        capacity = 1U;
    }
    const VkDeviceSize instanceBufferSize = checkedSceneInstanceBufferSize(capacity);

    frame.instanceCapacity = 0U;
    frame.instanceData = createBuffer(instanceBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const std::string frameName = "Frame " + std::to_string(frameIndex);
    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(frame.instanceData.buffer), frameName + " Instance Data Buffer");
    const std::string instanceAllocationName = frameName + " Instance Data Allocation";
    vmaSetAllocationName(allocator_, frame.instanceData.allocation, instanceAllocationName.c_str());
    frame.instanceData.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, instanceAllocationName, frame.instanceData.size);
    checkVk(vmaMapMemory(allocator_, frame.instanceData.allocation, &frame.instanceData.mapped), "vmaMapMemory instance data");
    frame.instanceCapacity = capacity;
}

void VulkanRenderer::updateFrameInstanceDataDescriptor(const std::size_t frameIndex) const {
    const FrameResources& frame = frames_[frameIndex];
    VkDescriptorBufferInfo instanceBufferInfo{};
    instanceBufferInfo.buffer = frame.instanceData.buffer;
    instanceBufferInfo.offset = 0;
    instanceBufferInfo.range = frame.instanceData.size;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sceneDescriptorSets_[frameIndex];
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &instanceBufferInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanRenderer::ensureSceneInstanceCapacity(FrameResources& frame, const std::size_t frameIndex, const std::size_t requiredCapacity) {
    if (requiredCapacity <= frame.instanceCapacity) {
        return;
    }

    std::size_t newCapacity = std::max(frame.instanceCapacity, std::size_t{1});
    while (newCapacity < requiredCapacity) {
        if (newCapacity > std::numeric_limits<std::size_t>::max() / 2U) {
            newCapacity = requiredCapacity;
            break;
        }
        newCapacity *= 2U;
    }

    destroyBuffer(frame.instanceData);
    createFrameInstanceDataBuffer(frame, frameIndex, newCapacity);
    updateFrameInstanceDataDescriptor(frameIndex);
    logger()->info("Grew frame {} scene instance capacity to {} items ({:.2f} MiB)", frameIndex, frame.instanceCapacity, bytesToMiB(frame.instanceData.size));
}

void VulkanRenderer::createFrameResources() {
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> sceneLayouts{};
    sceneLayouts.fill(sceneSetLayout_);
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = static_cast<std::uint32_t>(sceneLayouts.size());
    allocInfo.pSetLayouts = sceneLayouts.data();
    checkVk(vkAllocateDescriptorSets(device_, &allocInfo, sceneDescriptorSets_.data()), "vkAllocateDescriptorSets scene");

    const std::size_t initialInstanceCapacity = kInitialSceneInstanceCapacity;
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        FrameResources& frame = frames_[i];
        const std::string frameName = "Frame " + std::to_string(i);
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, handleToUint64(sceneDescriptorSets_[i]), frameName + " Scene Descriptor Set");

        VkCommandPoolCreateInfo framePoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        framePoolInfo.queueFamilyIndex = queueFamilies_.graphics.value();
        checkVk(vkCreateCommandPool(device_, &framePoolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool frame");
        setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(frame.commandPool), frameName + " Command Pool");

        VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdAlloc.commandPool = frame.commandPool;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(device_, &cmdAlloc, &frame.commandBuffer), "vkAllocateCommandBuffers frame");
        setObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, handleToUint64(frame.commandBuffer), frameName + " Command Buffer");
        frame.sceneUniforms = createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(frame.sceneUniforms.buffer), frameName + " Scene Uniform Buffer");
        const std::string uniformAllocationName = frameName + " Scene Uniform Allocation";
        vmaSetAllocationName(allocator_, frame.sceneUniforms.allocation, uniformAllocationName.c_str());
        frame.sceneUniforms.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, uniformAllocationName, frame.sceneUniforms.size);
        checkVk(vmaMapMemory(allocator_, frame.sceneUniforms.allocation, &frame.sceneUniforms.mapped), "vmaMapMemory scene uniforms");

        createFrameInstanceDataBuffer(frame, i, initialInstanceCapacity);

        frame.indirectCommands = createBuffer(sizeof(VkDrawIndexedIndirectCommand) * kSceneMeshBatchOrder.size(),
                                              VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(frame.indirectCommands.buffer), frameName + " Scene Indirect Commands Buffer");
        const std::string indirectAllocationName = frameName + " Scene Indirect Commands Allocation";
        vmaSetAllocationName(allocator_, frame.indirectCommands.allocation, indirectAllocationName.c_str());
        frame.indirectCommands.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, indirectAllocationName, frame.indirectCommands.size);
        checkVk(vmaMapMemory(allocator_, frame.indirectCommands.allocation, &frame.indirectCommands.mapped), "vmaMapMemory indirect commands");

        VkDescriptorBufferInfo sceneBufferInfo{};
        sceneBufferInfo.buffer = frame.sceneUniforms.buffer;
        sceneBufferInfo.offset = 0;
        sceneBufferInfo.range = sizeof(SceneUniforms);
        VkDescriptorBufferInfo instanceBufferInfo{};
        instanceBufferInfo.buffer = frame.instanceData.buffer;
        instanceBufferInfo.offset = 0;
        instanceBufferInfo.range = frame.instanceData.size;
        VkDescriptorImageInfo textureInfo{};
        textureInfo.sampler = textureSampler_;
        textureInfo.imageView = groundAlbedoTexture_.view;
        textureInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = sceneDescriptorSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &sceneBufferInfo;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = sceneDescriptorSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &textureInfo;
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet = sceneDescriptorSets_[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &instanceBufferInfo;
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore imageAvailable");
        setObjectName(VK_OBJECT_TYPE_SEMAPHORE, handleToUint64(frame.imageAvailable), frameName + " Image Available Semaphore");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), frameName + " In-Flight Fence");
    }
}

VulkanRenderer::Buffer VulkanRenderer::createBuffer(const VkDeviceSize size, const VkBufferUsageFlags usage, const VkMemoryPropertyFlags properties, const bool sharedGraphicsTransfer, const VmaAllocationCreateFlags hostAccessFlags) {
    Buffer buffer{};
    buffer.size = size;
    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = size;
    createInfo.usage = usage;
    const std::array<std::uint32_t, 2> queueFamilies{queueFamilies_.graphics.value(), queueFamilies_.transfer.value()};
    if (sharedGraphicsTransfer && queueFamilies[0] != queueFamilies[1]) {
        createInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilies.size());
        createInfo.pQueueFamilyIndices = queueFamilies.data();
    } else {
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.requiredFlags = properties;
    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
        allocationInfo.flags |= hostAccessFlags;
    }

    checkVk(vmaCreateBuffer(allocator_, &createInfo, &allocationInfo, &buffer.buffer, &buffer.allocation, nullptr), "vmaCreateBuffer");
    return buffer;
}

void VulkanRenderer::destroyBuffer(Buffer& buffer) {
    resourceRegistry_.unregisterResource(buffer.resourceId);
    buffer.resourceId = GpuResourceRegistry::kInvalidId;
    if (buffer.mapped != nullptr) {
        vmaUnmapMemory(allocator_, buffer.allocation);
    }
    if (buffer.buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation); }
    buffer = {};
}

void VulkanRenderer::destroyMeshUpload(MeshUpload& upload) {
    destroyBuffer(upload.indexStaging);
    destroyBuffer(upload.vertexStaging);
    destroyBuffer(upload.indices);
    destroyBuffer(upload.vertices);
    upload.cube = {};
    upload.sphere = {};
    upload.plane = {};
}

VkCommandBuffer VulkanRenderer::beginGraphicsUploadCommands() const {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = graphicsCommandPool_;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer), "vkAllocateCommandBuffers graphics upload");

    try {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer graphics upload");
    } catch (...) {
        vkFreeCommandBuffers(device_, graphicsCommandPool_, 1, &commandBuffer);
        throw;
    }
    return commandBuffer;
}

void VulkanRenderer::submitGraphicsUpload(VkCommandBuffer commandBuffer, std::vector<Buffer> stagingBuffers) {
    submitUploadBatch(graphicsQueue_, graphicsCommandPool_, commandBuffer, "graphics upload", std::move(stagingBuffers), false);
}

void VulkanRenderer::submitUploadBatch(const VkQueue queue,
                                       const VkCommandPool commandPool,
                                       const VkCommandBuffer commandBuffer,
                                       const char* operationName,
                                       std::vector<Buffer> stagingBuffers,
                                       const bool signalSemaphore) {
    PendingUploadBatch upload{};
    upload.commandPool = commandPool;
    upload.commandBuffer = commandBuffer;
    upload.stagingBuffers = std::move(stagingBuffers);
    bool queued = false;
    try {
        const std::string endOperation = std::string("vkEndCommandBuffer ") + operationName;
        checkVk(vkEndCommandBuffer(commandBuffer), endOperation.c_str());
        setObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, handleToUint64(commandBuffer), std::string("One-Shot ") + operationName + " Command Buffer");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        const std::string fenceOperation = std::string("vkCreateFence ") + operationName;
        checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &upload.fence), fenceOperation.c_str());
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(upload.fence), std::string("One-Shot ") + operationName + " Fence");

        if (signalSemaphore) {
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            const std::string semaphoreOperation = std::string("vkCreateSemaphore ") + operationName;
            checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &upload.signalSemaphore), semaphoreOperation.c_str());
            setObjectName(VK_OBJECT_TYPE_SEMAPHORE, handleToUint64(upload.signalSemaphore), std::string("One-Shot ") + operationName + " Semaphore");
        }

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        pendingUploads_.push_back(std::move(upload));
        queued = true;
        PendingUploadBatch& queuedUpload = pendingUploads_.back();
        if (queuedUpload.signalSemaphore != VK_NULL_HANDLE) {
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &queuedUpload.signalSemaphore;
        }
        const std::string submitOperation = std::string("vkQueueSubmit ") + operationName;
        checkVk(vkQueueSubmit(queue, 1, &submitInfo, queuedUpload.fence), submitOperation.c_str());
    } catch (...) {
        if (queued) {
            destroyPendingUpload(pendingUploads_.back());
            pendingUploads_.pop_back();
        } else {
            destroyPendingUpload(upload);
        }
        throw;
    }
}

void VulkanRenderer::retirePendingUploadResources(PendingUploadBatch& upload) {
    for (Buffer& stagingBuffer : upload.stagingBuffers) {
        destroyBuffer(stagingBuffer);
    }
    upload.stagingBuffers.clear();
    if (upload.fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, upload.fence, nullptr);
        upload.fence = VK_NULL_HANDLE;
    }
    if (upload.commandBuffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, upload.commandPool, 1, &upload.commandBuffer);
        upload.commandBuffer = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::destroyPendingUpload(PendingUploadBatch& upload) {
    retirePendingUploadResources(upload);
    if (upload.signalSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, upload.signalSemaphore, nullptr);
        upload.signalSemaphore = VK_NULL_HANDLE;
    }
    upload.commandPool = VK_NULL_HANDLE;
}

void VulkanRenderer::retireCompletedUploads() {
    for (std::size_t index = 0; index < pendingUploads_.size();) {
        PendingUploadBatch& upload = pendingUploads_[index];
        if (upload.fence == VK_NULL_HANDLE) {
            if (upload.signalSemaphore == VK_NULL_HANDLE) {
                if (index + 1U < pendingUploads_.size()) {
                    pendingUploads_[index] = std::move(pendingUploads_.back());
                }
                pendingUploads_.pop_back();
                continue;
            }
            ++index;
            continue;
        }

        const VkResult status = vkGetFenceStatus(device_, upload.fence);
        if (status == VK_NOT_READY) {
            ++index;
            continue;
        }
        checkVk(status, "vkGetFenceStatus upload");
        retirePendingUploadResources(upload);
        if (upload.signalSemaphore == VK_NULL_HANDLE) {
            if (index + 1U < pendingUploads_.size()) {
                pendingUploads_[index] = std::move(pendingUploads_.back());
            }
            pendingUploads_.pop_back();
            continue;
        }
        ++index;
    }
}

void VulkanRenderer::destroyFrameUploadWaitSemaphores(FrameResources& frame) {
    for (VkSemaphore& semaphore : frame.uploadWaitSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
        }
    }
    frame.uploadWaitSemaphores.clear();
}

void VulkanRenderer::collectPendingUploadWaitSemaphores(std::vector<VkSemaphore>& semaphores) const {
    semaphores.clear();
    semaphores.reserve(pendingUploads_.size());
    for (const PendingUploadBatch& upload : pendingUploads_) {
        if (upload.signalSemaphore == VK_NULL_HANDLE) {
            continue;
        }
        semaphores.push_back(upload.signalSemaphore);
    }
}

void VulkanRenderer::markUploadWaitSemaphoresQueued(FrameResources& frame,
                                                    const std::vector<VkSemaphore>& semaphores) {
    frame.uploadWaitSemaphores.reserve(frame.uploadWaitSemaphores.size() + semaphores.size());
    for (const VkSemaphore semaphore : semaphores) {
        frame.uploadWaitSemaphores.push_back(semaphore);
        for (PendingUploadBatch& upload : pendingUploads_) {
            if (upload.signalSemaphore == semaphore) {
                upload.signalSemaphore = VK_NULL_HANDLE;
                break;
            }
        }
    }
}

bool VulkanRenderer::formatSupportsLinearMipBlit(const VkFormat format) const {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
    constexpr VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                      VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (properties.optimalTilingFeatures & requiredFeatures) == requiredFeatures;
}

// Mip generation stays on the graphics queue for now: it avoids queue-family ownership transfers
// for exclusive sampled images and keeps the upload path deterministic during initialization.
void VulkanRenderer::generateMipmaps(VkCommandBuffer commandBuffer, ImageResource& image) const {
    std::int32_t mipWidth = static_cast<std::int32_t>(image.extent.width);
    std::int32_t mipHeight = static_cast<std::int32_t>(image.extent.height);

    for (std::uint32_t mipLevel = 1; mipLevel < image.mipLevels; ++mipLevel) {
        transitionImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        mipLevel - 1U, 1);

        VkImageBlit2 blit{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mipLevel - 1U;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[1] = {std::max(1, mipWidth / 2), std::max(1, mipHeight / 2), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mipLevel;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        VkBlitImageInfo2 blitInfo{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
        blitInfo.srcImage = image.image;
        blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blitInfo.dstImage = image.image;
        blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blitInfo.regionCount = 1;
        blitInfo.pRegions = &blit;
        blitInfo.filter = VK_FILTER_LINEAR;
        vkCmdBlitImage2(commandBuffer, &blitInfo);

        transitionImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        mipLevel - 1U, 1);

        mipWidth = std::max(1, mipWidth / 2);
        mipHeight = std::max(1, mipHeight / 2);
    }

    transitionImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    image.mipLevels - 1U, 1);
    image.syncState = imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::SampledImage);
}

VkCommandBuffer VulkanRenderer::beginUploadCommands() const {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = transferCommandPool_;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer), "vkAllocateCommandBuffers upload");
    try {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer upload");
    } catch (...) {
        vkFreeCommandBuffers(device_, transferCommandPool_, 1, &commandBuffer);
        throw;
    }
    return commandBuffer;
}

void VulkanRenderer::submitTransferUpload(VkCommandBuffer commandBuffer, std::vector<Buffer> stagingBuffers) {
    const bool needsQueueSemaphore = transferQueue_ != graphicsQueue_;
    submitUploadBatch(transferQueue_, transferCommandPool_, commandBuffer, "transfer upload", std::move(stagingBuffers), needsQueueSemaphore);
}

void VulkanRenderer::recordMeshUpload(VkCommandBuffer commandBuffer, const MeshUpload& upload) const {
    VkBufferCopy vertexCopy{};
    vertexCopy.size = upload.vertexSize;
    vkCmdCopyBuffer(commandBuffer, upload.vertexStaging.buffer, upload.vertices.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.size = upload.indexSize;
    vkCmdCopyBuffer(commandBuffer, upload.indexStaging.buffer, upload.indices.buffer, 1, &indexCopy);

    if (transferQueue_ == graphicsQueue_) {
        std::array<VkBufferMemoryBarrier2, 2> barriers{};
        barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].buffer = upload.vertices.buffer;
        barriers[0].offset = 0;
        barriers[0].size = upload.vertexSize;

        barriers[1] = barriers[0];
        barriers[1].buffer = upload.indices.buffer;
        barriers[1].size = upload.indexSize;
        barriers[1].dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;

        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
        dependency.pBufferMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }
}

const VulkanRenderer::GpuMesh& VulkanRenderer::meshFor(const SceneMeshId mesh) const {
    switch (mesh) {
    case SceneMeshId::Cube:
        return cube_;
    case SceneMeshId::Sphere:
        return sphere_;
    case SceneMeshId::GroundPlane:
        return plane_;
    }
    throw std::runtime_error("Unknown scene mesh id");
}

const VulkanRenderer::GpuMesh& VulkanRenderer::meshForBatch(const std::size_t meshIndex) const {
    if (meshIndex >= kSceneMeshBatchOrder.size()) {
        throw std::runtime_error("Scene mesh batch index out of range");
    }
    switch (kSceneMeshBatchOrder[meshIndex]) {
    case SceneMeshBatchId::Cube:
        return cube_;
    case SceneMeshBatchId::SphereHigh:
        return sphere_;
    case SceneMeshBatchId::SphereMedium:
        return sphereMedium_;
    case SceneMeshBatchId::SphereLow:
        return sphereLow_;
    case SceneMeshBatchId::GroundPlane:
        return plane_;
    }
    throw std::runtime_error("Unknown scene mesh batch id");
}


VulkanRenderer::MeshUpload VulkanRenderer::stageMeshUpload(const MeshData& cubeMesh, const MeshData& sphereMesh, const MeshData& sphereMediumMesh, const MeshData& sphereLowMesh, const MeshData& planeMesh) {
    MeshUpload upload{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(cubeMesh.vertices.size() + sphereMesh.vertices.size() + sphereMediumMesh.vertices.size() + sphereLowMesh.vertices.size() + planeMesh.vertices.size());
    indices.reserve(cubeMesh.indices.size() + sphereMesh.indices.size() + sphereMediumMesh.indices.size() + sphereLowMesh.indices.size() + planeMesh.indices.size());

    const auto appendMesh = [&](const MeshData& mesh) -> GpuMesh {
        if (mesh.vertices.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::runtime_error("Scene mesh vertex offset exceeds VkDrawIndexed vertexOffset range");
        }
        if (mesh.indices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error("Scene mesh index count exceeds uint32 range");
        }
        if (indices.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - mesh.indices.size()) {
            throw std::runtime_error("Scene geometry index buffer exceeds uint32 range");
        }
        const std::size_t firstVertex = vertices.size();
        if (firstVertex > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::runtime_error("Scene mesh vertex offset exceeds VkDrawIndexed vertexOffset range");
        }
        const std::size_t firstIndex = indices.size();
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
        return GpuMesh{
            static_cast<std::uint32_t>(mesh.indices.size()),
            static_cast<std::uint32_t>(firstIndex),
            static_cast<std::int32_t>(firstVertex),
        };
    };

    upload.cube = appendMesh(cubeMesh);
    upload.sphere = appendMesh(sphereMesh);
    upload.sphereMedium = appendMesh(sphereMediumMesh);
    upload.sphereLow = appendMesh(sphereLowMesh);
    upload.plane = appendMesh(planeMesh);
    upload.vertexSize = static_cast<VkDeviceSize>(vertices.size() * sizeof(Vertex));
    upload.indexSize = static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));

    try {
        upload.vertexStaging = createBuffer(upload.vertexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap vertexMap{allocator_, upload.vertexStaging.allocation, "vmaMapMemory vertex staging"};
            std::memcpy(vertexMap.get(), vertices.data(), static_cast<std::size_t>(upload.vertexSize));
        }

        upload.indexStaging = createBuffer(upload.indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap indexMap{allocator_, upload.indexStaging.allocation, "vmaMapMemory index staging"};
            std::memcpy(indexMap.get(), indices.data(), static_cast<std::size_t>(upload.indexSize));
        }

        upload.vertices = createBuffer(upload.vertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        upload.indices = createBuffer(upload.indexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        return upload;
    } catch (...) {
        destroyMeshUpload(upload);
        throw;
    }
}

void VulkanRenderer::createMeshes() {
    std::vector<Buffer> stagingBuffers;
    stagingBuffers.reserve(2);
    MeshUpload meshUpload{};
    try {
        meshUpload = stageMeshUpload(createCubeMesh(), createUvSphereMesh(32, 64), createUvSphereMesh(16, 32), createUvSphereMesh(8, 16), createPlaneMesh(12.0f, 12.0f));

        VkCommandBuffer uploadCommands = beginUploadCommands();
        recordMeshUpload(uploadCommands, meshUpload);
        stagingBuffers.push_back(meshUpload.indexStaging);
        meshUpload.indexStaging = {};
        stagingBuffers.push_back(meshUpload.vertexStaging);
        meshUpload.vertexStaging = {};
        submitTransferUpload(uploadCommands, std::move(stagingBuffers));
    } catch (...) {
        destroyMeshUpload(meshUpload);
        throw;
    }

    sceneVertexBuffer_ = meshUpload.vertices;
    sceneIndexBuffer_ = meshUpload.indices;
    cube_ = meshUpload.cube;
    sphere_ = meshUpload.sphere;
    sphereMedium_ = meshUpload.sphereMedium;
    sphereLow_ = meshUpload.sphereLow;
    plane_ = meshUpload.plane;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::Cube)] = cube_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh)] = sphere_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium)] = sphereMedium_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::SphereLow)] = sphereLow_.indexCount / 3U;
    sceneMeshTriangleCounts_[sceneMeshBatchIndex(SceneMeshBatchId::GroundPlane)] = plane_.indexCount / 3U;
    meshUpload.vertices = {};
    meshUpload.indices = {};
    meshUpload.cube = {};
    meshUpload.sphere = {};
    meshUpload.plane = {};

    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(sceneVertexBuffer_.buffer), "Scene Geometry Vertex Buffer");
    vmaSetAllocationName(allocator_, sceneVertexBuffer_.allocation, "Scene Geometry Vertex Allocation");
    sceneVertexBuffer_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Scene Geometry Vertex Buffer", sceneVertexBuffer_.size);
    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(sceneIndexBuffer_.buffer), "Scene Geometry Index Buffer");
    vmaSetAllocationName(allocator_, sceneIndexBuffer_.allocation, "Scene Geometry Index Allocation");
    sceneIndexBuffer_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Scene Geometry Index Buffer", sceneIndexBuffer_.size);
}

void VulkanRenderer::createTonemapDescriptorSet() {
    if (tonemapDescriptorSet_ == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &tonemapSetLayout_;
        checkVk(vkAllocateDescriptorSets(device_, &allocInfo, &tonemapDescriptorSet_), "vkAllocateDescriptorSets tonemap");
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, handleToUint64(tonemapDescriptorSet_), "Tonemap Descriptor Set");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = linearSampler_;
    imageInfo.imageView = hdr_.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = tonemapDescriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanRenderer::createTimestampQueries() {
    if (!config_.gpuTimestamps) {
        timestampsEnabled_ = false;
        timestampValidBits_ = 0;
        deviceInfo_.timestampQueries = false;
        stats_.gpuTimestampsValid = false;
        return;
    }

    const QueueFamilies families = findQueueFamilies(physicalDevice_);
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueProperties.data());
    timestampValidBits_ = queueProperties[families.graphics.value()].timestampValidBits;
    timestampsEnabled_ = timestampValidBits_ > 0U;
    deviceInfo_.timestampQueries = timestampsEnabled_;
    if (!timestampsEnabled_) {
        logger()->warn("Graphics queue does not support timestamp queries");
        return;
    }

    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = static_cast<std::uint32_t>(kMaxFramesInFlight) * kTimestampQueriesPerFrame;
    checkVk(vkCreateQueryPool(device_, &queryInfo, nullptr, &timestampQueryPool_), "vkCreateQueryPool timestamps");
    setObjectName(VK_OBJECT_TYPE_QUERY_POOL, handleToUint64(timestampQueryPool_), "Frame Timestamp Query Pool");
}

void VulkanRenderer::createFrameGraph() {
    frameGraph_ = {};

    const FrameGraph::ResourceHandle depth = frameGraph_.addResource({"Depth Image", FrameGraphResourceKind::Image, false});
    const FrameGraph::ResourceHandle hdr = frameGraph_.addResource({"HDR Color Image", FrameGraphResourceKind::Image, false});
    const FrameGraph::ResourceHandle swapchain = frameGraph_.addResource({"Swapchain Image", FrameGraphResourceKind::Image, true});

    const bool useDepthPrepass = resolveDepthPrepass(config_.depthPrepassMode);
    FrameGraph::PassHandle depthPass{};
    if (useDepthPrepass) {
        depthPass = frameGraph_.addPass({"Depth Prepass", {0.16f, 0.42f, 0.18f, 1.0f}});
        frameGraph_.write(depthPass, depth, FrameGraphUsage::DepthAttachment);
    }

    const FrameGraph::PassHandle hdrPass = frameGraph_.addPass({"HDR Scene Pass", {0.18f, 0.32f, 0.95f, 1.0f}});
    if (useDepthPrepass) {
        frameGraph_.read(hdrPass, depth, FrameGraphUsage::DepthAttachment);
    } else {
        frameGraph_.write(hdrPass, depth, FrameGraphUsage::DepthAttachment);
    }
    frameGraph_.write(hdrPass, hdr, FrameGraphUsage::ColorAttachment);

    const FrameGraph::PassHandle tonemapPass = frameGraph_.addPass({config_.debugOverlay ? "Tonemap + ImGui Pass" : "Tonemap Pass", {0.95f, 0.58f, 0.16f, 1.0f}});
    frameGraph_.read(tonemapPass, hdr, FrameGraphUsage::SampledImage);
    frameGraph_.write(tonemapPass, swapchain, FrameGraphUsage::ColorAttachment);
    const FrameGraph::PassHandle screenshotPass = frameGraph_.addPass({"Screenshot Readback", {0.36f, 0.74f, 0.95f, 1.0f}});
    frameGraph_.read(screenshotPass, swapchain, FrameGraphUsage::TransferSource);
    frameGraph_.setFinalUsage(swapchain, FrameGraphUsage::Present);

    frameGraph_.compile();
    const bool depthEdgesMatch = useDepthPrepass
        ? frameGraph_.hasEdge(depthPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment)
              && frameGraph_.hasEdge(hdrPass, depth, FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment)
        : frameGraph_.hasEdge(hdrPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment);
    if (!depthEdgesMatch) {
        throw std::runtime_error("FrameGraph depth edges do not match configured prepass mode");
    }
    if (!frameGraph_.hasEdge(screenshotPass, swapchain, FrameGraphAccess::Read, FrameGraphUsage::TransferSource)) {
        throw std::runtime_error("FrameGraph screenshot readback edge is missing");
    }
    frameGraphResources_ = {depth, hdr, swapchain};
    frameGraphPasses_ = {depthPass, hdrPass, tonemapPass, screenshotPass};
    logger()->info("Compiled frame graph (depth prepass {}): {} passes, {} resources, {} edges",
                   useDepthPrepass ? "on" : "off", frameGraph_.passCount(), frameGraph_.resourceCount(), frameGraph_.edgeCount());
}

void VulkanRenderer::createImGui() {
#if VOLKENGINE_ENABLE_IMGUI
    if (imguiInitialized_) {
        return;
    }
    if (!config_.debugOverlay) {
        imguiInitialized_ = false;
        return;
    }

    bool contextCreated = false;
    bool glfwBackendInitialized = false;
    bool vulkanBackendInitAttempted = false;
    const auto cleanupPartialImGui = [&]() noexcept {
        if (vulkanBackendInitAttempted) {
            ImGui_ImplVulkan_Shutdown();
        }
        if (glfwBackendInitialized) {
            ImGui_ImplGlfw_Shutdown();
        }
        if (contextCreated) {
            ImGui::DestroyContext();
        }
    };

    try {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        contextCreated = true;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplGlfw_InitForVulkan(window_.handle(), false)) {
            throw std::runtime_error("Failed to initialize Dear ImGui GLFW backend");
        }
        glfwBackendInitialized = true;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = instance_;
        initInfo.PhysicalDevice = physicalDevice_;
        initInfo.Device = device_;
        initInfo.QueueFamily = queueFamilies_.graphics.value();
        initInfo.Queue = graphicsQueue_;
        initInfo.DescriptorPoolSize = 32;
        initInfo.MinImageCount = swapchainMinImageCount_;
        initInfo.ImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
        initInfo.PipelineCache = pipelineCache_;
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat_;
        initInfo.CheckVkResultFn = [](VkResult result) { checkVk(result, "Dear ImGui Vulkan backend"); };
        initInfo.MinAllocationSize = 1024U * 1024U;

        vulkanBackendInitAttempted = true;
        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("Failed to initialize Dear ImGui Vulkan backend");
        }

        imguiInitialized_ = true;
        imguiDiagnosticsValid_ = false;
        imguiDiagnosticsRefreshSeconds_ = 0.0;
        imguiMinImageCount_ = initInfo.MinImageCount;
        imguiImageCount_ = initInfo.ImageCount;
        imguiSwapchainFormat_ = swapchainFormat_;
        contextCreated = false;
        glfwBackendInitialized = false;
        vulkanBackendInitAttempted = false;
        logger()->info("Initialized Dear ImGui debug overlay");
    } catch (...) {
        cleanupPartialImGui();
        throw;
    }

#else
    imguiInitialized_ = false;
#endif
}

void VulkanRenderer::shutdownImGui() {
#if VOLKENGINE_ENABLE_IMGUI
    if (!imguiInitialized_) {
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized_ = false;
    imguiMinImageCount_ = 0;
    imguiImageCount_ = 0;
    imguiSwapchainFormat_ = VK_FORMAT_UNDEFINED;
    imguiDiagnosticsValid_ = false;
    imguiDiagnosticsRefreshSeconds_ = 0.0;
#endif
}

void VulkanRenderer::beginImGuiFrame(const double frameDeltaMs) {
#if VOLKENGINE_ENABLE_IMGUI
    if (!config_.debugOverlay || !imguiInitialized_) {
        return;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos({12.0f, 12.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    imguiDiagnosticsRefreshSeconds_ -= frameDeltaMs * 0.001;
    if (!imguiDiagnosticsValid_ || imguiDiagnosticsRefreshSeconds_ <= 0.0) {
        imguiResourceStats_ = resourceRegistry_.stats();
        imguiMemoryUsageBytes_ = 0;
        imguiMemoryBudgetBytes_ = 0;
        if (memoryBudgetEnabled_) {
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(allocator_, budgets.data());
            for (std::uint32_t heapIndex = 0; heapIndex < physicalDeviceMemoryProperties_.memoryHeapCount; ++heapIndex) {
                imguiMemoryUsageBytes_ += budgets[heapIndex].usage;
                imguiMemoryBudgetBytes_ += budgets[heapIndex].budget;
            }
        }
        imguiDiagnosticsRefreshSeconds_ = kImGuiDiagnosticsRefreshIntervalSeconds;
        imguiDiagnosticsValid_ = true;
    }
    const GpuResourceRegistry::Stats& resourceStats = imguiResourceStats_;
    if (ImGui::Begin("VolkEngine renderer stats", nullptr, flags)) {
        ImGui::TextUnformatted("VolkEngine Vulkan renderer");
        ImGui::Text("GPU: %s (%s)", deviceInfo_.adapterName.c_str(), gpuClassName(deviceInfo_.discreteGpu));
        ImGui::Text("Vulkan: %u.%u.%u  max2D: %u",
                    deviceInfo_.apiVersionMajor, deviceInfo_.apiVersionMinor,
                    deviceInfo_.apiVersionPatch, deviceInfo_.maxImageDimension2D);
        ImGui::Separator();
        ImGui::Text("Frame delta: %.3f ms", frameDeltaMs);
        ImGui::Text("CPU render-submit: %.3f ms", stats_.cpuFrameMs);
        ImGui::Text("CPU phases: scene %.3f / prepare %.3f / record %.3f / submit %.3f ms",
                    stats_.cpuSceneBuildMs, stats_.cpuPrepareMs, stats_.cpuCommandRecordMs, stats_.cpuQueueSubmitMs);
        if (stats_.gpuTimestampsValid) {
            ImGui::Text("GPU frame: %.3f ms (depth %.3f / HDR %.3f / final %.3f)",
                        stats_.gpuFrameMs, stats_.gpuDepthPrepassMs, stats_.gpuHdrSceneMs, stats_.gpuFinalPassMs);
        } else {
            ImGui::TextUnformatted("GPU frame: pending/unavailable");
        }
        ImGui::Text("Draws: %u  Culled: %u  Grid tiles: %u/%u accepted, %u culled, %u intersected",
                    stats_.drawCalls, stats_.culledDrawCalls, stats_.gridTilesAccepted, stats_.gridTileCount,
                    stats_.gridTilesCulled, stats_.gridTilesIntersected);
        ImGui::Text("Grid visibility cache: %s  Work records: %u",
                    stats_.gridVisibilityCacheHit ? "hit" : "miss", stats_.gridVisibilityWorkItems);
        ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(stats_.triangleCount));
        ImGui::Text("Sphere LOD instances: %u high / %u medium / %u low",
                    stats_.sphereLodHighCount, stats_.sphereLodMediumCount, stats_.sphereLodLowCount);
        ImGui::Text("Exposure: %.2f  VSync: %s  Depth prepass: %s (%s)", config_.exposure, config_.vsync ? "on" : "off", stats_.depthPrepassEnabled ? "on" : "off", depthPrepassModeName(config_.depthPrepassMode));
        ImGui::Text("Scene: %u items, %u visible, %u mesh batches, %u scene passes, %s",
                    stats_.sceneItemCount, stats_.visibleItemCount, stats_.meshBatchCount, stats_.scenePassCount,
                    stats_.indirectSceneDraws ? "multi-draw indirect" : "direct batched");
        ImGui::Text("Upload sync: %s  max indirect draws: %u",
                    transferUploadSyncName(deviceInfo_.transferUploadSync), deviceInfo_.maxDrawIndirectCount);
        ImGui::Text("Instance storage: %u capacity (%.2f MiB)",
                    stats_.sceneInstanceCapacity, stats_.sceneInstanceBufferMiB);
        ImGui::Text("Swapchain: %ux%u  images: %u/%u  present: %s",
                    swapchainExtent_.width, swapchainExtent_.height, imguiImageCount_, imguiMinImageCount_, presentModeName(presentMode_).data());
        ImGui::Text("Validation: %s", validationEnabled_ ? "enabled" : (config_.validation ? "requested unavailable" : "off"));
        if (memoryBudgetEnabled_) {
            ImGui::Text("VMA memory budget: %.1f / %.1f MiB",
                        bytesToMiB(imguiMemoryUsageBytes_),
                        bytesToMiB(imguiMemoryBudgetBytes_));
        } else {
            ImGui::TextUnformatted("VMA memory budget: unavailable");
        }
        ImGui::Text("Descriptor indexing: %s  bindless sampled-image support: %s",
                    deviceInfo_.descriptorIndexing ? "supported" : "unavailable",
                    deviceInfo_.bindlessSampledImagesSupported ? "supported" : "unavailable");
        ImGui::Text("Texture sampling: anisotropy %s (%.1fx)",
                    deviceInfo_.samplerAnisotropy ? "enabled" : "off",
                    deviceInfo_.maxSamplerAnisotropy);
        ImGui::Text("GPU resources: %u live (%u buffers, %u images, %u imported), %.2f MiB",
                    resourceStats.liveResources, resourceStats.buffers, resourceStats.images,
                    resourceStats.importedImages, bytesToMiB(resourceStats.bytes));
        ImGui::Text("Resource bytes: buffers %.2f MiB, owned images %.2f MiB, imported images %.2f MiB",
                    bytesToMiB(resourceStats.bufferBytes), bytesToMiB(resourceStats.ownedImageBytes),
                    bytesToMiB(resourceStats.importedImageBytes));
    }
    ImGui::End();
    ImGui::Render();
#else
    (void)frameDeltaMs;
#endif
}

void VulkanRenderer::renderImGui(const VkCommandBuffer commandBuffer) const {
#if VOLKENGINE_ENABLE_IMGUI
    if (!config_.debugOverlay || !imguiInitialized_) {
        return;
    }
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
#else
    (void)commandBuffer;
#endif
}

void VulkanRenderer::updateUniforms(FrameResources& frame, const Camera& camera, const Mat4& viewProjection, const double elapsedSeconds) {
    const Vec3 position = camera.position();
    const Vec3 lightDirection = normalize(Vec3{-0.45f, -1.0f, -0.35f});
    const SceneUniforms uniforms{
        viewProjection,
        {position.x, position.y, position.z, static_cast<float>(elapsedSeconds)},
        {lightDirection.x, lightDirection.y, lightDirection.z, 0.0f},
        {1.0f, 0.93f, 0.82f, 8.0f},
        {0.46f, 0.58f, 0.82f, 0.055f},
        {0.14f, 0.12f, 0.10f, 0.030f},
    };
    std::memcpy(frame.sceneUniforms.mapped, &uniforms, sizeof(uniforms));
}

void VulkanRenderer::restoreFrameFenceAfterSubmitFailure(FrameResources& frame, const std::size_t frameIndex, const VkResult submitResult) {
    VkFence replacement = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const VkResult fenceResult = vkCreateFence(device_, &fenceInfo, nullptr, &replacement);
    if (fenceResult == VK_SUCCESS) {
        const VkFence oldFence = frame.inFlight;
        frame.inFlight = replacement;
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), "Frame " + std::to_string(frameIndex) + " In-Flight Fence");
        if (oldFence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, oldFence, nullptr);
        }
        logger()->error("vkQueueSubmit frame failed with VkResult {}; restored frame {} fence to signaled before throwing",
                        static_cast<int>(submitResult),
                        frameIndex);
    } else {
        logger()->error("vkQueueSubmit frame failed with VkResult {}; failed to restore frame {} fence to signaled state (vkCreateFence returned {})",
                        static_cast<int>(submitResult),
                        frameIndex,
                        static_cast<int>(fenceResult));
    }
}

void VulkanRenderer::draw(const Camera& camera, const double elapsedSeconds, const double frameDeltaMs) {
    FrameResources& frame = frames_[frameIndex_];
    checkVk(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences frame");
    retireDeferredPipelineSets();
    readBackGpuTimestamp(static_cast<std::uint32_t>(frameIndex_));
    destroyFrameUploadWaitSemaphores(frame);
    retireCompletedUploads();
    pollShaderHotReload(elapsedSeconds);

    std::uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        checkVk(acquire, "vkAcquireNextImageKHR");
    }
    bool screenshotThisFrame = false;
    VkExtent2D screenshotExtent{};
    VkFormat screenshotFormat = VK_FORMAT_UNDEFINED;
    std::filesystem::path screenshotPath;
    bool screenshotRequested = false;
    {
        const std::scoped_lock lock{screenshotRequestMutex_};
        if (screenshotPending_) {
            screenshotPath = std::move(screenshotPath_);
            screenshotPath_.clear();
            screenshotPending_ = false;
            screenshotRequested = true;
        }
    }
    if (screenshotRequested) {
        if (!swapchainTransferSrcSupported_) {
            logger()->warn("Screenshot requested but swapchain images do not support TRANSFER_SRC usage");
        } else if (!screenshotFormatSupported()) {
            logger()->warn("Screenshot requested but swapchain format {} is not BGRA8/RGBA8 UNORM", static_cast<int>(swapchainFormat_));
        } else {
            const VkDeviceSize screenshotBytes = static_cast<VkDeviceSize>(swapchainExtent_.width) * static_cast<VkDeviceSize>(swapchainExtent_.height) * 4U;
            if (screenshotReadback_.buffer != VK_NULL_HANDLE && screenshotReadback_.size != screenshotBytes) {
                destroyBuffer(screenshotReadback_);
            }
            if (screenshotReadback_.buffer == VK_NULL_HANDLE) {
                screenshotReadback_ = createBuffer(screenshotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                   false, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
                setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(screenshotReadback_.buffer), "Screenshot Readback Buffer");
                vmaSetAllocationName(allocator_, screenshotReadback_.allocation, "Screenshot Readback Allocation");
                screenshotReadback_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Screenshot Readback Buffer", screenshotReadback_.size);
            }
            screenshotThisFrame = true;
            screenshotExtent = swapchainExtent_;
            screenshotFormat = swapchainFormat_;
        }
    }

    const auto cpuStart = std::chrono::steady_clock::now();
    const SceneRenderList& renderItems = sceneRenderer_.build(elapsedSeconds, config_.materialGridRows, config_.materialGridColumns, config_.materialGridTileRows, config_.materialGridTileColumns);
    const auto cpuSceneEnd = std::chrono::steady_clock::now();
    const Mat4 projection = camera.projectionMatrix();
    const Mat4 viewProjection = projection * camera.viewMatrix();
    SceneVisibilityPlan visibility = planSceneVisibility(camera, projection, viewProjection, renderItems);
    ensureSceneInstanceCapacity(frame, frameIndex_, visibility.visibleItemCount);
    updateUniforms(frame, camera, viewProjection, elapsedSeconds);
    checkVk(vkResetCommandPool(device_, frame.commandPool, 0), "vkResetCommandPool frame");
    beginImGuiFrame(frameDeltaMs);
    const auto cpuPrepareEnd = std::chrono::steady_clock::now();
    auto cpuRecordEnd = cpuPrepareEnd;
    auto cpuEnd = cpuPrepareEnd;
    pendingUploadWaitSemaphores_.clear();
    VkSemaphore renderFinished = swapchainRenderFinishedSemaphores_[imageIndex];
    const FrameImageSyncSnapshot imageSyncSnapshot = captureFrameImageSyncState(imageIndex);
    bool frameCommandsSubmitted = false;
    try {
        recordCommandBuffer(frame, imageIndex, renderItems, visibility, screenshotThisFrame ? &screenshotReadback_ : nullptr);
        cpuRecordEnd = std::chrono::steady_clock::now();

        collectPendingUploadWaitSemaphores(pendingUploadWaitSemaphores_);
        submitWaitSemaphores_.clear();
        submitWaitSemaphores_.reserve(pendingUploadWaitSemaphores_.size() + 1U);
        submitWaitSemaphores_.push_back(frame.imageAvailable);
        submitWaitSemaphores_.insert(submitWaitSemaphores_.end(), pendingUploadWaitSemaphores_.begin(), pendingUploadWaitSemaphores_.end());

        submitWaitStages_.clear();
        submitWaitStages_.reserve(submitWaitSemaphores_.size());
        submitWaitStages_.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        submitWaitStages_.insert(submitWaitStages_.end(), pendingUploadWaitSemaphores_.size(), VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.waitSemaphoreCount = static_cast<std::uint32_t>(submitWaitSemaphores_.size());
        submitInfo.pWaitSemaphores = submitWaitSemaphores_.data();
        submitInfo.pWaitDstStageMask = submitWaitStages_.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
        checkVk(vkResetFences(device_, 1, &frame.inFlight), "vkResetFences frame");
        const VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frame.inFlight);
        if (submitResult != VK_SUCCESS) {
            restoreFrameFenceAfterSubmitFailure(frame, frameIndex_, submitResult);
            checkVk(submitResult, "vkQueueSubmit frame");
        }
        frameCommandsSubmitted = true;
    } catch (...) {
        if (!frameCommandsSubmitted) {
            restoreFrameImageSyncState(imageIndex, imageSyncSnapshot);
        }
        throw;
    }
    markUploadWaitSemaphoresQueued(frame, pendingUploadWaitSemaphores_);
    frame.submittedOnce = true;
    cpuEnd = std::chrono::steady_clock::now();


    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult present = vkQueuePresentKHR(presentQueue_, &presentInfo);
    if (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR) {
        swapchainStates_[imageIndex] = {};
    }
    if (screenshotThisFrame) {
        checkVk(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences screenshot capture");
        try {
            writeScreenshotPpm(screenshotReadback_, screenshotExtent, screenshotFormat, screenshotPath);
            logger()->info("Saved screenshot {}", screenshotPath.string());
            destroyBuffer(screenshotReadback_);
        } catch (...) {
            destroyBuffer(screenshotReadback_);
            throw;
        }
    }
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR || window_.consumeFramebufferResized()) {
        recreateSwapchain();
    } else if (present != VK_SUCCESS) {
        checkVk(present, "vkQueuePresentKHR");
    }


    stats_.cpuFrameMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count();
    stats_.cpuSceneBuildMs = std::chrono::duration<double, std::milli>(cpuSceneEnd - cpuStart).count();
    stats_.cpuPrepareMs = std::chrono::duration<double, std::milli>(cpuPrepareEnd - cpuSceneEnd).count();
    stats_.cpuCommandRecordMs = std::chrono::duration<double, std::milli>(cpuRecordEnd - cpuPrepareEnd).count();
    stats_.cpuQueueSubmitMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuRecordEnd).count();
    stats_.frameDeltaMs = frameDeltaMs;
    stats_.elapsedSeconds = elapsedSeconds;
    frameIndex_ = (frameIndex_ + 1U) % kMaxFramesInFlight;
}

void VulkanRenderer::readBackGpuTimestamp(const std::uint32_t frameIndex) {
    if (!timestampsEnabled_ || !frames_[frameIndex].submittedOnce) {
        stats_.gpuTimestampsValid = false;
        return;
    }
    std::array<std::uint64_t, kTimestampQueriesPerFrame> timestamps{};
    const std::uint32_t queryBase = frameIndex * kTimestampQueriesPerFrame;
    const VkResult result = vkGetQueryPoolResults(device_, timestampQueryPool_, queryBase, kTimestampQueriesPerFrame,
                                                  sizeof(timestamps), timestamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
        const auto deltaTicks = [this](const std::uint64_t begin, const std::uint64_t end) -> std::uint64_t {
            if (timestampValidBits_ >= 64U) {
                return end - begin;
            }
            const std::uint64_t mask = (1ULL << timestampValidBits_) - 1ULL;
            return ((end & mask) - (begin & mask)) & mask;
        };
        const double tickToMs = static_cast<double>(physicalDeviceProperties_.limits.timestampPeriod) / 1'000'000.0;
        if (frames_[frameIndex].submittedDepthPrepass) {
            stats_.gpuDepthPrepassMs = static_cast<double>(deltaTicks(timestamps[kTimestampFrameStart], timestamps[kTimestampDepthEnd])) * tickToMs;
            stats_.gpuHdrSceneMs = static_cast<double>(deltaTicks(timestamps[kTimestampDepthEnd], timestamps[kTimestampHdrEnd])) * tickToMs;
        } else {
            stats_.gpuDepthPrepassMs = 0.0;
            stats_.gpuHdrSceneMs = static_cast<double>(deltaTicks(timestamps[kTimestampFrameStart], timestamps[kTimestampHdrEnd])) * tickToMs;
        }
        stats_.gpuFinalPassMs = static_cast<double>(deltaTicks(timestamps[kTimestampHdrEnd], timestamps[kTimestampFinalEnd])) * tickToMs;
        stats_.gpuFrameMs = static_cast<double>(deltaTicks(timestamps[kTimestampFrameStart], timestamps[kTimestampFinalEnd])) * tickToMs;
        stats_.gpuTimestampsValid = true;
    } else {
        stats_.gpuTimestampsValid = false;
    }
}

bool VulkanRenderer::screenshotFormatSupported() const {
    return swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapchainFormat_ == VK_FORMAT_R8G8B8A8_UNORM;
}

void VulkanRenderer::recordScreenshotCopy(const VkCommandBuffer commandBuffer, const std::uint32_t imageIndex, const Buffer& readback) {
    transitionImageTracked(commandBuffer, swapchainImages_[imageIndex], swapchainStates_[imageIndex],
                           imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::TransferSource),
                           VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {swapchainExtent_.width, swapchainExtent_.height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &region);
}

void VulkanRenderer::writeScreenshotPpm(const Buffer& readback, const VkExtent2D extent, const VkFormat format, const std::filesystem::path& path) const {
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM) {
        throw std::runtime_error("Screenshot capture only supports BGRA8/RGBA8 swapchain formats");
    }

    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    ScopedVmaMap mappedReadback{allocator_, readback.allocation, "vmaMapMemory screenshot readback"};
    const auto* src = static_cast<const std::uint8_t*>(mappedReadback.get());

    std::filesystem::path tempPath = path;
    tempPath += ".tmp";

    try {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("Failed to open screenshot temp file: " + tempPath.string());
        }

        file << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
        std::vector<std::uint8_t> row(static_cast<std::size_t>(extent.width) * 3U);
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            const auto* srcRow = src + (static_cast<std::size_t>(y) * extent.width * 4U);
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                const std::size_t srcOffset = static_cast<std::size_t>(x) * 4U;
                const std::size_t dstOffset = static_cast<std::size_t>(x) * 3U;
                if (format == VK_FORMAT_B8G8R8A8_UNORM) {
                    row[dstOffset + 0U] = srcRow[srcOffset + 2U];
                    row[dstOffset + 1U] = srcRow[srcOffset + 1U];
                    row[dstOffset + 2U] = srcRow[srcOffset + 0U];
                } else {
                    row[dstOffset + 0U] = srcRow[srcOffset + 0U];
                    row[dstOffset + 1U] = srcRow[srcOffset + 1U];
                    row[dstOffset + 2U] = srcRow[srcOffset + 2U];
                }
            }
            file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
        file.close();
        if (!file) {
            throw std::runtime_error("Failed to write screenshot temp file: " + tempPath.string());
        }
    } catch (...) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        throw;
    }

    std::error_code publishError;
    std::filesystem::rename(tempPath, path, publishError);
    if (!publishError) {
        return;
    }

    std::error_code existsError;
    const bool targetExists = std::filesystem::exists(path, existsError);
    if (!existsError && targetExists) {
        std::filesystem::path backupPath;
        bool backupCreated = false;
        const auto backupSeed = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint32_t attempt = 0; attempt < 16U && !backupCreated; ++attempt) {
            backupPath = path;
            backupPath += ".bak.";
            backupPath += std::to_string(backupSeed);
            backupPath += ".";
            backupPath += std::to_string(attempt);
            std::error_code backupExistsError;
            const bool backupExists = std::filesystem::exists(backupPath, backupExistsError);
            if (backupExistsError || backupExists) {
                continue;
            }

            std::error_code backupError;
            std::filesystem::rename(path, backupPath, backupError);
            backupCreated = !backupError;
        }

        if (backupCreated) {
            publishError.clear();
            std::filesystem::rename(tempPath, path, publishError);
            if (!publishError) {
                std::error_code removeBackupError;
                std::filesystem::remove(backupPath, removeBackupError);
                return;
            }

            std::error_code restoreError;
            std::filesystem::rename(backupPath, path, restoreError);
            if (restoreError) {
                throw std::runtime_error("Failed to publish screenshot file: " + path.string()
                                         + " (old screenshot kept at " + backupPath.string()
                                         + ", temp kept at " + tempPath.string() + "): " + publishError.message());
            }
        }
    }

    throw std::runtime_error("Failed to publish screenshot file: " + path.string()
                             + " (temp kept at " + tempPath.string() + "): " + publishError.message());
}

VulkanRenderer::SceneVisibilityPlan VulkanRenderer::planSceneVisibility(const Camera& camera, const Mat4& projection, const Mat4& viewProjection, const SceneRenderList& renderItems) {
    static_assert(kSceneMeshBatchOrder.size() == kSceneMeshBatchCount);
    SceneVisibilityPlan plan{};
    const Frustum frustum = extractFrustumPlanes(viewProjection);
    auto& visibleSceneWork = visibleSceneWorkScratch_;
    visibleSceneWork.clear();
    const auto& meshTriangleCounts = sceneMeshTriangleCounts_;

    const Vec3 cameraPosition = camera.position();
    const Vec3 cameraForward = camera.forward();
    const float projectionScaleY = projection.m[5] < 0.0f ? -projection.m[5] : projection.m[5];
    const std::size_t sphereHighBatchIndex = sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh);
    const std::size_t sphereMediumBatchIndex = sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium);
    const std::size_t sphereLowBatchIndex = sceneMeshBatchIndex(SceneMeshBatchId::SphereLow);
    const auto sphereLodBatchIndex = [&](const Vec3 boundsCenter, const float projectedBoundsRadius, const float conservativeDepthRadius) {
        const float viewDepth = std::max(dot(boundsCenter - cameraPosition, cameraForward) - conservativeDepthRadius, 0.001f);
        const float projectedRadiusNdc = (projectedBoundsRadius * projectionScaleY) / viewDepth;
        if (projectedRadiusNdc >= 0.035f) {
            return sphereHighBatchIndex;
        }
        if (projectedRadiusNdc >= 0.012f) {
            return sphereMediumBatchIndex;
        }
        return sphereLowBatchIndex;
    };
    const auto meshBatchIndexFor = [&](const SceneMeshId mesh, const Vec3 boundsCenter, const float projectedBoundsRadius, const float conservativeDepthRadius) {
        if (mesh == SceneMeshId::Sphere) {
            return sphereLodBatchIndex(boundsCenter, projectedBoundsRadius, conservativeDepthRadius);
        }
        return sceneMeshBatchIndex(mesh);
    };
    const auto acceptVisibleItem = [&](const std::size_t itemIndex, const SceneRenderItem& item) {
        const std::size_t meshIndex = meshBatchIndexFor(item.mesh, item.boundsCenter, item.boundsRadius, 0.0f);
        visibleSceneWork.push_back(VisibleSceneWork{VisibleSceneWork::Kind::Item,
                                                    static_cast<std::uint8_t>(meshIndex),
                                                    static_cast<std::uint32_t>(itemIndex)});
        ++plan.visibleItemCount;
        ++plan.meshInstanceCounts[meshIndex];
        plan.sceneTriangleCount += meshTriangleCounts[meshIndex];
    };
    const auto acceptHomogeneousTile = [&](const SceneGridTile& tile, const std::size_t tileIndex) -> bool {
        if (!tile.homogeneousMesh ||
            tileIndex > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
            tile.itemCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - plan.visibleItemCount)) {
            return false;
        }
        const std::size_t meshIndex = meshBatchIndexFor(tile.commonMesh, tile.boundsCenter, tile.maxItemBoundsRadius, tile.boundsRadius);
        if (tile.itemCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - plan.meshInstanceCounts[meshIndex])) {
            return false;
        }
        visibleSceneWork.push_back(VisibleSceneWork{VisibleSceneWork::Kind::HomogeneousGridTile,
                                                    static_cast<std::uint8_t>(meshIndex),
                                                    static_cast<std::uint32_t>(tileIndex)});
        plan.visibleItemCount += static_cast<std::uint32_t>(tile.itemCount);
        plan.meshInstanceCounts[meshIndex] += static_cast<std::uint32_t>(tile.itemCount);
        plan.sceneTriangleCount += static_cast<std::uint64_t>(meshTriangleCounts[meshIndex]) * static_cast<std::uint64_t>(tile.itemCount);
        return true;
    };
    const auto cullItem = [&](const std::size_t itemIndex) {
        const SceneRenderItem& item = renderItems[itemIndex];
        if (sphereOutsideFrustum(frustum, item.boundsCenter, item.boundsRadius)) {
            ++plan.culledDrawCalls;
            return;
        }
        acceptVisibleItem(itemIndex, item);
    };
    const auto cullRange = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t itemIndex = begin; itemIndex < end; ++itemIndex) {
            cullItem(itemIndex);
        }
    };

    plan.gridRange = renderItems.materialGridRange();
    const std::vector<SceneGridTile>& gridTiles = renderItems.materialGridTiles();
    plan.gridItemCount = static_cast<std::size_t>(plan.gridRange.rows) * static_cast<std::size_t>(plan.gridRange.columns);
    const bool gridTilesCoverRange = renderItems.materialGridTilesCoverRange();
    plan.useGridTiles = plan.gridRange.valid &&
                        gridTilesCoverRange &&
                        plan.gridRange.firstItem <= renderItems.size() &&
                        plan.gridItemCount <= (renderItems.size() - plan.gridRange.firstItem);
    if (plan.useGridTiles) {
        cullRange(0, plan.gridRange.firstItem);
        plan.gridWorkBegin = visibleSceneWork.size();
        const bool reuseCachedGridVisibility = gridVisibilityCache_.valid &&
                                               gridVisibilityCache_.tileRevision == renderItems.materialGridTileRevision() &&
                                               sameGridRange(gridVisibilityCache_.range, plan.gridRange) &&
                                               gridVisibilityCache_.tileCount == gridTiles.size() &&
                                               sameMatrix(gridVisibilityCache_.viewProjection, viewProjection);
        if (reuseCachedGridVisibility) {
            plan.gridWorkEnd = plan.gridWorkBegin;
            plan.visibleItemCount += gridVisibilityCache_.visibleItemCount;
            plan.sceneTriangleCount += gridVisibilityCache_.sceneTriangleCount;
            plan.culledDrawCalls += gridVisibilityCache_.culledDrawCalls;
            plan.gridTileCount += gridVisibilityCache_.gridTileCount;
            plan.gridTilesAccepted += gridVisibilityCache_.gridTilesAccepted;
            plan.gridTilesCulled += gridVisibilityCache_.gridTilesCulled;
            plan.gridTilesIntersected += gridVisibilityCache_.gridTilesIntersected;
            for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
                plan.meshInstanceCounts[meshIndex] += gridVisibilityCache_.meshInstanceCounts[meshIndex];
            }
            plan.gridVisibilityCacheHit = true;
            plan.gridVisibilityWorkItems = gridVisibilityCache_.workItemCount;
        } else {
            plan.gridWorkBegin = visibleSceneWork.size();
            const std::uint32_t gridVisibleBegin = plan.visibleItemCount;
            const std::uint64_t gridTriangleBegin = plan.sceneTriangleCount;
            const std::uint32_t gridCulledBegin = plan.culledDrawCalls;
            const std::uint32_t gridTileBegin = plan.gridTileCount;
            const std::uint32_t gridAcceptedBegin = plan.gridTilesAccepted;
            const std::uint32_t gridCulledTileBegin = plan.gridTilesCulled;
            const std::uint32_t gridIntersectedBegin = plan.gridTilesIntersected;
            const auto meshCountBegin = plan.meshInstanceCounts;
            for (std::size_t tileIndex = 0; tileIndex < gridTiles.size(); ++tileIndex) {
                const SceneGridTile& tile = gridTiles[tileIndex];
                const auto visitTileItems = [&](const auto& visitor) {
                    for (std::uint32_t row = tile.rowBegin; row < tile.rowEnd; ++row) {
                        const std::size_t rowBase = plan.gridRange.firstItem + (static_cast<std::size_t>(row) * plan.gridRange.columns);
                        for (std::uint32_t column = tile.columnBegin; column < tile.columnEnd; ++column) {
                            visitor(rowBase + column);
                        }
                    }
                };
                ++plan.gridTileCount;
                const FrustumSphereClassification tileVisibility = classifySphereAgainstFrustum(frustum, tile.boundsCenter, tile.boundsRadius);
                if (tileVisibility == FrustumSphereClassification::Outside) {
                    plan.culledDrawCalls += static_cast<std::uint32_t>(tile.itemCount);
                    ++plan.gridTilesCulled;
                    continue;
                }
                if (tileVisibility == FrustumSphereClassification::Inside) {
                    ++plan.gridTilesAccepted;
                    if (!acceptHomogeneousTile(tile, tileIndex)) {
                        visitTileItems([&](const std::size_t itemIndex) {
                            acceptVisibleItem(itemIndex, renderItems[itemIndex]);
                        });
                    }
                    continue;
                }
                ++plan.gridTilesIntersected;
                visitTileItems(cullItem);
            }

            plan.gridWorkEnd = visibleSceneWork.size();
            gridVisibilityCache_.valid = false;
            gridVisibilityCache_.tileRevision = renderItems.materialGridTileRevision();
            gridVisibilityCache_.range = plan.gridRange;
            gridVisibilityCache_.tileCount = gridTiles.size();
            gridVisibilityCache_.viewProjection = viewProjection;
            gridVisibilityCache_.workItemCount = static_cast<std::uint32_t>(plan.gridWorkEnd - plan.gridWorkBegin);
            for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
                gridVisibilityCache_.meshInstanceCounts[meshIndex] = plan.meshInstanceCounts[meshIndex] - meshCountBegin[meshIndex];
                gridVisibilityCache_.instanceDataByMesh[meshIndex].clear();
                gridVisibilityCache_.instanceDataByMesh[meshIndex].reserve(gridVisibilityCache_.meshInstanceCounts[meshIndex]);
            }
            gridVisibilityCache_.visibleItemCount = plan.visibleItemCount - gridVisibleBegin;
            gridVisibilityCache_.sceneTriangleCount = plan.sceneTriangleCount - gridTriangleBegin;
            gridVisibilityCache_.culledDrawCalls = plan.culledDrawCalls - gridCulledBegin;
            gridVisibilityCache_.gridTileCount = plan.gridTileCount - gridTileBegin;
            gridVisibilityCache_.gridTilesAccepted = plan.gridTilesAccepted - gridAcceptedBegin;
            gridVisibilityCache_.gridTilesCulled = plan.gridTilesCulled - gridCulledTileBegin;
            gridVisibilityCache_.gridTilesIntersected = plan.gridTilesIntersected - gridIntersectedBegin;
            plan.gridVisibilityWorkItems = gridVisibilityCache_.workItemCount;
        }
        cullRange(plan.gridRange.firstItem + plan.gridItemCount, renderItems.size());
    } else {
        gridVisibilityCache_.valid = false;
        cullRange(0, renderItems.size());
    }

    return plan;
}

void VulkanRenderer::recordCommandBuffer(FrameResources& frame, const std::uint32_t imageIndex, const SceneRenderList& renderItems, const SceneVisibilityPlan& visibility, const Buffer* screenshotReadback) {
    static_assert(kSceneMeshBatchOrder.size() == kSceneMeshBatchCount);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer frame");

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, swapchainExtent_};
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    if (timestampsEnabled_) {
        const std::uint32_t queryBase = static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame;
        vkCmdResetQueryPool(frame.commandBuffer, timestampQueryPool_, queryBase, kTimestampQueriesPerFrame);
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueryPool_, queryBase + kTimestampFrameStart);
    }


    auto& visibleSceneWork = visibleSceneWorkScratch_;
    auto* instanceData = static_cast<InstanceData*>(frame.instanceData.mapped);

    struct MeshBatch {
        const GpuMesh* mesh = nullptr;
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
    };
    std::array<MeshBatch, kSceneMeshBatchOrder.size()> meshBatches{};
    std::array<std::uint32_t, kSceneMeshBatchOrder.size()> meshFirstInstances{};
    std::array<std::uint32_t, kSceneMeshBatchOrder.size()> meshWriteCursors{};
    const SceneGridRange& gridRange = visibility.gridRange;
    const std::vector<SceneGridTile>& gridTiles = renderItems.materialGridTiles();
    const bool useGridTiles = visibility.useGridTiles;
    const bool gridVisibilityCacheHit = visibility.gridVisibilityCacheHit;
    const std::size_t gridWorkBegin = visibility.gridWorkBegin;
    const std::size_t gridWorkEnd = visibility.gridWorkEnd;
    std::uint32_t visibleInstanceCount = 0;
    for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
        meshFirstInstances[meshIndex] = visibleInstanceCount;
        meshWriteCursors[meshIndex] = visibleInstanceCount;
        visibleInstanceCount += visibility.meshInstanceCounts[meshIndex];
    }
    if (visibleInstanceCount > frame.instanceCapacity) {
        throw std::runtime_error("Scene visibility plan exceeds frame instance capacity");
    }

    const auto makeInstanceData = [](const SceneRenderItem& item) {
        return InstanceData{item.model, item.material.albedoRoughness, item.material.emissiveMetallic, item.material.flags};
    };
    const auto writeCachedGridInstances = [&](const std::size_t meshIndex) {
        const std::vector<InstanceData>& cachedInstances = gridVisibilityCache_.instanceDataByMesh[meshIndex];
        if (!cachedInstances.empty()) {
            std::memcpy(instanceData + meshWriteCursors[meshIndex], cachedInstances.data(), sizeof(InstanceData) * cachedInstances.size());
            meshWriteCursors[meshIndex] += static_cast<std::uint32_t>(cachedInstances.size());
        }
    };
    const auto writeVisibleItem = [&](const std::size_t itemIndex, const std::size_t meshIndex, const bool cacheGridInstance) {
        const InstanceData data = makeInstanceData(renderItems[itemIndex]);
        instanceData[meshWriteCursors[meshIndex]++] = data;
        if (cacheGridInstance) {
            gridVisibilityCache_.instanceDataByMesh[meshIndex].push_back(data);
        }
    };
    const auto materializeWorkRange = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t workIndex = begin; workIndex < end; ++workIndex) {
            const VisibleSceneWork& work = visibleSceneWork[workIndex];
            const std::size_t meshIndex = work.meshIndex;
            const bool cacheGridInstance = useGridTiles && !gridVisibilityCacheHit && workIndex >= gridWorkBegin && workIndex < gridWorkEnd;
            if (work.kind == VisibleSceneWork::Kind::Item) {
                writeVisibleItem(work.index, meshIndex, cacheGridInstance);
                continue;
            }
            const SceneGridTile& tile = gridTiles[work.index];
            for (std::uint32_t row = tile.rowBegin; row < tile.rowEnd; ++row) {
                const std::size_t rowBase = gridRange.firstItem + (static_cast<std::size_t>(row) * gridRange.columns);
                for (std::uint32_t column = tile.columnBegin; column < tile.columnEnd; ++column) {
                    writeVisibleItem(rowBase + column, meshIndex, cacheGridInstance);
                }
            }
        }
    };

    if (gridVisibilityCacheHit) {
        materializeWorkRange(0, gridWorkBegin);
        for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
            writeCachedGridInstances(meshIndex);
        }
        materializeWorkRange(gridWorkBegin, visibleSceneWork.size());
    } else {
        materializeWorkRange(0, visibleSceneWork.size());
    }
    if (useGridTiles && !gridVisibilityCacheHit) {
        gridVisibilityCache_.valid = true;
    }


    std::uint32_t sceneDrawCalls = 0;
    for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
        if (visibility.meshInstanceCounts[meshIndex] == 0U) {
            continue;
        }
        meshBatches[sceneDrawCalls++] = MeshBatch{&meshForBatch(meshIndex), meshFirstInstances[meshIndex], visibility.meshInstanceCounts[meshIndex]};
    }

    if (indirectSceneDrawsEnabled_ && sceneDrawCalls > 0U) {
        auto* indirectCommands = static_cast<VkDrawIndexedIndirectCommand*>(frame.indirectCommands.mapped);
        for (std::uint32_t batchIndex = 0; batchIndex < sceneDrawCalls; ++batchIndex) {
            const MeshBatch& batch = meshBatches[batchIndex];
            indirectCommands[batchIndex] = VkDrawIndexedIndirectCommand{
                batch.mesh->indexCount,
                batch.instanceCount,
                batch.mesh->firstIndex,
                batch.mesh->vertexOffset,
                batch.firstInstance,
            };
        }
    }

    const VkDeviceSize offset = 0;
    const auto drawSceneBatches = [&] {
        if (sceneDrawCalls == 0U) {
            return;
        }
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &sceneVertexBuffer_.buffer, &offset);
        vkCmdBindIndexBuffer(frame.commandBuffer, sceneIndexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
        if (indirectSceneDrawsEnabled_) {
            vkCmdDrawIndexedIndirect(frame.commandBuffer, frame.indirectCommands.buffer, 0, sceneDrawCalls, sizeof(VkDrawIndexedIndirectCommand));
            return;
        }
        for (std::uint32_t batchIndex = 0; batchIndex < sceneDrawCalls; ++batchIndex) {
            const MeshBatch& batch = meshBatches[batchIndex];
            vkCmdDrawIndexed(frame.commandBuffer, batch.mesh->indexCount, batch.instanceCount,
                             batch.mesh->firstIndex, batch.mesh->vertexOffset, batch.firstInstance);
        }
    };

    const VkDescriptorSet sceneSet = sceneDescriptorSets_[frameIndex_];
    const bool useDepthPrepass = resolveDepthPrepass(config_.depthPrepassMode);
    const FrameGraph::PassDesc* depthPass = useDepthPrepass ? &frameGraph_.pass(frameGraphPasses_.depthPrepass) : nullptr;
    const FrameGraph::PassDesc& hdrPass = frameGraph_.pass(frameGraphPasses_.hdrScene);
    const FrameGraph::PassDesc& tonemapPass = frameGraph_.pass(frameGraphPasses_.tonemap);
    const FrameGraph::PassDesc& screenshotPass = frameGraph_.pass(frameGraphPasses_.screenshotReadback);


    if (useDepthPrepass) {
        {
            const DebugLabelScope depthLabel{*this, frame.commandBuffer, depthPass->name, depthPass->debugColor};
            transitionImageTracked(frame.commandBuffer, depth_.image, depth_.syncState,
                                   imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment),
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo depthPrepassAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depthPrepassAttachment.imageView = depth_.view;
            depthPrepassAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthPrepassAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthPrepassAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthPrepassAttachment.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo depthPrepassInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            depthPrepassInfo.renderArea.offset = {0, 0};
            depthPrepassInfo.renderArea.extent = swapchainExtent_;
            depthPrepassInfo.layerCount = 1;
            depthPrepassInfo.colorAttachmentCount = 0;
            depthPrepassInfo.pDepthAttachment = &depthPrepassAttachment;
            vkCmdBeginRendering(frame.commandBuffer, &depthPrepassInfo);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPrepassPipeline_);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, 0, 1, &sceneSet, 0, nullptr);
            drawSceneBatches();
            vkCmdEndRendering(frame.commandBuffer);
        }

        transitionImageTracked(frame.commandBuffer, depth_.image, depth_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment),
                               VK_IMAGE_ASPECT_DEPTH_BIT);
    } else {
        transitionImageTracked(frame.commandBuffer, depth_.image, depth_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment),
                               VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (timestampsEnabled_) {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueryPool_,
                             static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame + kTimestampDepthEnd);
    }

    {
        const DebugLabelScope hdrLabel{*this, frame.commandBuffer, hdrPass.name, hdrPass.debugColor};
        transitionImageTracked(frame.commandBuffer, hdr_.image, hdr_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment),
                               VK_IMAGE_ASPECT_COLOR_BIT);

        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = hdr_.view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{0.02f, 0.025f, 0.035f, 1.0f}};

        VkRenderingAttachmentInfo sceneDepthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sceneDepthAttachment.imageView = depth_.view;
        sceneDepthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sceneDepthAttachment.loadOp = useDepthPrepass ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        sceneDepthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        sceneDepthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderInfo.renderArea.offset = {0, 0};
        renderInfo.renderArea.extent = swapchainExtent_;
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;
        renderInfo.pDepthAttachment = &sceneDepthAttachment;
        vkCmdBeginRendering(frame.commandBuffer, &renderInfo);

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, useDepthPrepass ? scenePipeline_ : sceneNoPrepassPipeline_);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, 0, 1, &sceneSet, 0, nullptr);
        drawSceneBatches();
        vkCmdEndRendering(frame.commandBuffer);

        transitionImageTracked(frame.commandBuffer, hdr_.image, hdr_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::SampledImage),
                               VK_IMAGE_ASPECT_COLOR_BIT);
        if (timestampsEnabled_) {
            vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueryPool_,
                                 static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame + kTimestampHdrEnd);
        }
    }

    {
        const DebugLabelScope tonemapLabel{*this, frame.commandBuffer, tonemapPass.name, tonemapPass.debugColor};
        transitionImageTracked(frame.commandBuffer, swapchainImages_[imageIndex], swapchainStates_[imageIndex],
                               imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment),
                               VK_IMAGE_ASPECT_COLOR_BIT);

        VkRenderingAttachmentInfo swapchainAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        swapchainAttachment.imageView = swapchainImageViews_[imageIndex];
        swapchainAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        swapchainAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo tonemapRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        tonemapRenderInfo.renderArea.offset = {0, 0};
        tonemapRenderInfo.renderArea.extent = swapchainExtent_;
        tonemapRenderInfo.layerCount = 1;
        tonemapRenderInfo.colorAttachmentCount = 1;
        tonemapRenderInfo.pColorAttachments = &swapchainAttachment;
        vkCmdBeginRendering(frame.commandBuffer, &tonemapRenderInfo);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipelineLayout_, 0, 1, &tonemapDescriptorSet_, 0, nullptr);
        vkCmdPushConstants(frame.commandBuffer, tonemapPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(config_.exposure), &config_.exposure);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        renderImGui(frame.commandBuffer);
        vkCmdEndRendering(frame.commandBuffer);
    }

    if (screenshotReadback != nullptr) {
        const DebugLabelScope screenshotLabel{*this, frame.commandBuffer, screenshotPass.name, screenshotPass.debugColor};
        recordScreenshotCopy(frame.commandBuffer, imageIndex, *screenshotReadback);
    }

    transitionImageTracked(frame.commandBuffer, swapchainImages_[imageIndex], swapchainStates_[imageIndex],
                           finalImageSyncStateFor(frameGraph_.finalUsage(frameGraphResources_.swapchain)),
                           VK_IMAGE_ASPECT_COLOR_BIT);

    if (timestampsEnabled_) {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueryPool_,
                             static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame + kTimestampFinalEnd);
    }

    checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer frame");
    const std::uint32_t scenePassCount = useDepthPrepass ? 2U : 1U;
    frame.submittedDepthPrepass = useDepthPrepass;
    frame.submittedScenePassCount = scenePassCount;
    stats_.depthPrepassEnabled = useDepthPrepass;
    stats_.scenePassCount = scenePassCount;
    stats_.sceneItemCount = static_cast<unsigned>(renderItems.size());
    stats_.visibleItemCount = visibility.visibleItemCount;
    stats_.sceneInstanceCapacity = static_cast<unsigned>(frame.instanceCapacity);
    stats_.sceneInstanceBufferMiB = bytesToMiB(frame.instanceData.size);
    stats_.meshBatchCount = sceneDrawCalls;
    const std::uint32_t sceneDrawCommandCount = (indirectSceneDrawsEnabled_ && sceneDrawCalls > 0U) ? 1U : sceneDrawCalls;
    stats_.drawCalls = (sceneDrawCommandCount * scenePassCount) + 1U;
    stats_.culledDrawCalls = visibility.culledDrawCalls;
    stats_.gridTileCount = visibility.gridTileCount;
    stats_.gridTilesCulled = visibility.gridTilesCulled;
    stats_.gridTilesAccepted = visibility.gridTilesAccepted;
    stats_.gridTilesIntersected = visibility.gridTilesIntersected;
    stats_.sphereLodHighCount = visibility.meshInstanceCounts[sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh)];
    stats_.sphereLodMediumCount = visibility.meshInstanceCounts[sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium)];
    stats_.sphereLodLowCount = visibility.meshInstanceCounts[sceneMeshBatchIndex(SceneMeshBatchId::SphereLow)];
    stats_.gridVisibilityCacheHit = visibility.gridVisibilityCacheHit;
    stats_.gridVisibilityWorkItems = visibility.gridVisibilityWorkItems;
    stats_.indirectSceneDraws = indirectSceneDrawsEnabled_ && sceneDrawCalls > 0U;
    stats_.triangleCount = (visibility.sceneTriangleCount * scenePassCount) + 1ULL;
}

VulkanRenderer::ImageSyncState VulkanRenderer::imageSyncStateFor(const FrameGraphAccess access, const FrameGraphUsage usage) {
    switch (usage) {
    case FrameGraphUsage::ColorAttachment:
        return ImageSyncState{
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            access == FrameGraphAccess::Write ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
        };
    case FrameGraphUsage::DepthAttachment:
        return ImageSyncState{
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            access == FrameGraphAccess::Write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        };
    case FrameGraphUsage::SampledImage:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Sampled image frame-graph usage must be read-only");
        }
        return ImageSyncState{
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        };
    case FrameGraphUsage::TransferSource:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Transfer-source usage must be read-only");
        }
        return ImageSyncState{
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
        };
    case FrameGraphUsage::Present:
        return finalImageSyncStateFor(usage);
    }
    throw std::runtime_error("Unknown frame-graph image usage");
}

VulkanRenderer::ImageSyncState VulkanRenderer::finalImageSyncStateFor(const FrameGraphUsage usage) {
    if (usage != FrameGraphUsage::Present) {
        throw std::runtime_error("Unsupported frame-graph final image usage");
    }
    return ImageSyncState{VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
}

VulkanRenderer::FrameImageSyncSnapshot VulkanRenderer::captureFrameImageSyncState(const std::uint32_t imageIndex) const {
    return FrameImageSyncSnapshot{
        depth_.syncState,
        hdr_.syncState,
        swapchainStates_[imageIndex],
    };
}

void VulkanRenderer::restoreFrameImageSyncState(const std::uint32_t imageIndex, const FrameImageSyncSnapshot& snapshot) {
    depth_.syncState = snapshot.depth;
    hdr_.syncState = snapshot.hdr;
    swapchainStates_[imageIndex] = snapshot.swapchain;
}

void VulkanRenderer::transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect,
                                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, const std::uint32_t baseMipLevel, const std::uint32_t levelCount) const {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void VulkanRenderer::transitionImageTracked(VkCommandBuffer cmd, VkImage image, ImageSyncState& syncState, ImageSyncState newState, VkImageAspectFlags aspect,
                                            const std::uint32_t baseMipLevel, const std::uint32_t levelCount) const {
    transitionImage(cmd, image, syncState.layout, newState.layout, aspect,
                    syncState.stage, syncState.access, newState.stage, newState.access,
                    baseMipLevel, levelCount);
    syncState = newState;
}

void VulkanRenderer::cleanupSwapchain() {
    destroyImage(hdr_);
    destroyImage(depth_);
    for (VkImageView view : swapchainImageViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    for (const std::uint32_t resourceId : swapchainResourceIds_) {
        resourceRegistry_.unregisterResource(resourceId);
    }
    swapchainResourceIds_.clear();
    swapchainImageViews_.clear();
    for (VkSemaphore semaphore : swapchainRenderFinishedSemaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    swapchainRenderFinishedSemaphores_.clear();
    swapchainImages_.clear();
    swapchainStates_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::recreateSwapchain() {
    VkExtent2D extent = window_.framebufferExtent();
    while ((extent.width == 0U || extent.height == 0U) && !window_.shouldClose()) {
        window_.waitEvents();
        extent = window_.framebufferExtent();
    }
    if (extent.width == 0U || extent.height == 0U) {
        return;
    }

    const VkFormat previousSwapchainFormat = swapchainFormat_;
    const VkFormat previousHdrFormat = hdr_.format;
    const VkFormat previousDepthFormat = depth_.format;
    const bool hadImGui = imguiInitialized_;

    checkVk(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle recreate swapchain");
    if (hadImGui) {
        shutdownImGui();
    }
    cleanupSwapchain();
    createSwapchain();
    createImageViews();
    createDepthResources();
    createHdrResources();
    if (hadImGui) {
        createImGui();
    }

    const bool pipelineFormatsChanged = previousSwapchainFormat != swapchainFormat_ || previousHdrFormat != hdr_.format || previousDepthFormat != depth_.format;
    if (pipelineFormatsChanged || depthPrepassPipeline_ == VK_NULL_HANDLE || scenePipeline_ == VK_NULL_HANDLE || sceneNoPrepassPipeline_ == VK_NULL_HANDLE || tonemapPipeline_ == VK_NULL_HANDLE) {
        if (tonemapPipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, tonemapPipeline_, nullptr); tonemapPipeline_ = VK_NULL_HANDLE; }
        if (tonemapPipelineLayout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, tonemapPipelineLayout_, nullptr); tonemapPipelineLayout_ = VK_NULL_HANDLE; }
        if (depthPrepassPipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, depthPrepassPipeline_, nullptr); depthPrepassPipeline_ = VK_NULL_HANDLE; }
        if (scenePipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, scenePipeline_, nullptr); scenePipeline_ = VK_NULL_HANDLE; }
        if (sceneNoPrepassPipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device_, sceneNoPrepassPipeline_, nullptr); sceneNoPrepassPipeline_ = VK_NULL_HANDLE; }
        if (scenePipelineLayout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, scenePipelineLayout_, nullptr); scenePipelineLayout_ = VK_NULL_HANDLE; }
        createPipelines();
    }
    createTonemapDescriptorSet();
    const GpuResourceRegistry::Stats resourceStats = resourceRegistry_.stats();
    logger()->info("Recreated swapchain; tracked GPU resources: {} live ({} buffers, {} images, {} imported), {:.2f} MiB estimated (buffers {:.2f}, owned images {:.2f}, imported images {:.2f})",
                   resourceStats.liveResources, resourceStats.buffers, resourceStats.images,
                   resourceStats.importedImages, bytesToMiB(resourceStats.bytes), bytesToMiB(resourceStats.bufferBytes),
                   bytesToMiB(resourceStats.ownedImageBytes), bytesToMiB(resourceStats.importedImageBytes));
}

} // namespace ve
