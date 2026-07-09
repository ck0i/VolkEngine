#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VulkanRenderer::Impl::Impl(Window& window, EngineConfig config)
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

VulkanRenderer::Impl::~Impl() {
    cleanupResources(true);
}

void VulkanRenderer::Impl::cleanupResources(const bool persistPipelineCache) noexcept {
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
    destroyImage(groundNormalTexture_);

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
    if (normalTextureSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, normalTextureSampler_, nullptr);
        normalTextureSampler_ = VK_NULL_HANDLE;
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

void VulkanRenderer::Impl::waitIdle() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (timestampsEnabled_) {
            const std::uint32_t lastFrameIndex = static_cast<std::uint32_t>((frameIndex_ + kMaxFramesInFlight - 1U) % kMaxFramesInFlight);
            readBackGpuTimestamp(lastFrameIndex);
        }
    }
}

} // namespace ve
