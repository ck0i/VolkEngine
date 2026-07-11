#include "renderer/vulkan/VulkanRendererImpl.hpp"

#include <stdexcept>

namespace ve {

VulkanRenderer::Impl::Impl(Window& window, EngineConfig config,
                           const ReferenceAssetBundle& referenceAssets)
    : window_(window), config_(std::move(config)) {
    resourceOwner_.referenceAssets = &referenceAssets;
    if (!isValidExposure(config_.exposure)) {
        throw std::runtime_error("Renderer exposure must be positive and finite");
    }
    try {
    createInstance();
    createDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    if (config_.gpuVisibilityValidation && !indirectSceneDrawsEnabled_) {
        throw std::runtime_error(
            "GPU visibility validation requires indirect scene draws");
    }
    createAllocator();
    loadDebugUtils();
    createCommandPools();
    frameOwner_.pendingUploads.reserve(8);
    pipelineOwner_.retiredSets.reserve(2);
    createSwapchain();
    createImageViews();
    createFrameGraph(false);
    realizeFrameGraphResources();
    createTextureResources();
    createSampler();
    createDescriptorLayouts();
    createPipelineCache();
    createPipelines();
    createMeshes();
    createFrameResources();
    createTonemapDescriptorSet();
    createTimestampQueries();
    if (config_.debugOverlay) {
        createImGui();
    }
    logger()->info("Renderer enabled: dynamicRendering {} sync2 {} timestamps {} validation {} debugMarkers {} memoryBudget {} indirectSceneDraws {} imguiOverlay {} transferUploadSync {}; supported: descriptorIndexing {} bindlessSampledImages {} multiDrawIndirect {} drawIndirectFirstInstance {} samplerAnisotropy {} maxAnisotropy {:.1f} maxDrawIndirectCount {}",
                   capabilityName(deviceOwner_.info.dynamicRendering), capabilityName(deviceOwner_.info.synchronization2),
                   capabilityName(deviceOwner_.info.timestampQueries), capabilityName(deviceOwner_.info.validationEnabled),
                   capabilityName(deviceOwner_.info.debugMarkers), capabilityName(deviceOwner_.info.memoryBudget),
                   capabilityName(deviceOwner_.info.indirectSceneDraws), capabilityName(imguiOwner_.initialized),
                   transferUploadSyncName(deviceOwner_.info.transferUploadSync), capabilityName(deviceOwner_.info.descriptorIndexing),
                   capabilityName(deviceOwner_.info.bindlessSampledImagesSupported), capabilityName(deviceOwner_.info.multiDrawIndirect),
                   capabilityName(deviceOwner_.info.drawIndirectFirstInstance), capabilityName(deviceOwner_.info.samplerAnisotropy),
                   deviceOwner_.info.maxSamplerAnisotropy, deviceOwner_.info.maxDrawIndirectCount);
    const GpuResourceRegistry::Stats resourceStats = resourceOwner_.registry.stats();
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
    if (deviceOwner_.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(deviceOwner_.device);
    }

    shutdownImGui();

    for (PendingUploadBatch& upload : frameOwner_.pendingUploads) {
        destroyPendingUpload(upload);
    }
    frameOwner_.pendingUploads.clear();

    if (frameOwner_.timestampQueryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(deviceOwner_.device, frameOwner_.timestampQueryPool, nullptr);
        frameOwner_.timestampQueryPool = VK_NULL_HANDLE;
    }

    destroyBuffer(readback_.buffer());
    destroyBuffer(resourceOwner_.sceneIndexBuffer);
    destroyBuffer(resourceOwner_.sceneVertexBuffer);
    destroyBuffer(resourceOwner_.clusterHierarchy);
    destroyBuffer(resourceOwner_.meshClusterRanges);
    destroyBuffer(resourceOwner_.clusterData);
    for (ImageResource& texture : resourceOwner_.materialTextures) {
        destroyImage(texture);
    }
    resourceOwner_.materialTextures.clear();

    for (FrameResources& frame : frameOwner_.frames) {
        destroyFrameUploadWaitSemaphores(frame);
        destroyBuffer(frame.sceneUniforms);
        destroyBuffer(frame.instanceData);
        destroyBuffer(frame.cullCandidates);
        destroyBuffer(frame.cullUniforms);
        destroyBuffer(frame.visibleInstanceIndices);
        destroyBuffer(frame.cullCounters);
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(deviceOwner_.device, frame.imageAvailable, nullptr);
            frame.imageAvailable = VK_NULL_HANDLE;
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(deviceOwner_.device, frame.inFlight, nullptr);
            frame.inFlight = VK_NULL_HANDLE;
        }
        destroyBuffer(frame.indirectCommands);
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(deviceOwner_.device, frame.commandPool, nullptr);
            frame.commandPool = VK_NULL_HANDLE;
        }
    }

    cleanupSwapchain();
    for (RetiredPipelineSet& retired : pipelineOwner_.retiredSets) {
        destroyPipelineSet(retired.pipelines);
    }
    pipelineOwner_.retiredSets.clear();

    PipelineSet activePipelines = detachActivePipelineSet();
    destroyPipelineSet(activePipelines);
    if (pipelineOwner_.cache != VK_NULL_HANDLE) {
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
        vkDestroyPipelineCache(deviceOwner_.device, pipelineOwner_.cache, nullptr);
        pipelineOwner_.cache = VK_NULL_HANDLE;
    }
    if (resourceOwner_.descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(deviceOwner_.device, resourceOwner_.descriptorPool, nullptr);
        resourceOwner_.descriptorPool = VK_NULL_HANDLE;
    }
    if (resourceOwner_.cullSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(deviceOwner_.device,
                                     resourceOwner_.cullSetLayout, nullptr);
        resourceOwner_.cullSetLayout = VK_NULL_HANDLE;
    }
    if (resourceOwner_.depthPyramidSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(deviceOwner_.device,
                                     resourceOwner_.depthPyramidSetLayout,
                                     nullptr);
        resourceOwner_.depthPyramidSetLayout = VK_NULL_HANDLE;
    }
    if (resourceOwner_.tonemapSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(deviceOwner_.device, resourceOwner_.tonemapSetLayout, nullptr);
        resourceOwner_.tonemapSetLayout = VK_NULL_HANDLE;
    }
    if (resourceOwner_.sceneSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(deviceOwner_.device, resourceOwner_.sceneSetLayout, nullptr);
        resourceOwner_.sceneSetLayout = VK_NULL_HANDLE;
    }
    if (resourceOwner_.linearSampler != VK_NULL_HANDLE) {
        vkDestroySampler(deviceOwner_.device, resourceOwner_.linearSampler, nullptr);
        resourceOwner_.linearSampler = VK_NULL_HANDLE;
    }
    if (resourceOwner_.textureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(deviceOwner_.device, resourceOwner_.textureSampler, nullptr);
        resourceOwner_.textureSampler = VK_NULL_HANDLE;
    }
    if (resourceOwner_.normalTextureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(deviceOwner_.device, resourceOwner_.normalTextureSampler, nullptr);
        resourceOwner_.normalTextureSampler = VK_NULL_HANDLE;
    }
    if (resourceOwner_.ormTextureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(deviceOwner_.device, resourceOwner_.ormTextureSampler, nullptr);
        resourceOwner_.ormTextureSampler = VK_NULL_HANDLE;
    }
    if (frameOwner_.graphicsCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(deviceOwner_.device, frameOwner_.graphicsCommandPool, nullptr);
        frameOwner_.graphicsCommandPool = VK_NULL_HANDLE;
    }
    if (frameOwner_.transferCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(deviceOwner_.device, frameOwner_.transferCommandPool, nullptr);
        frameOwner_.transferCommandPool = VK_NULL_HANDLE;
    }
    if (deviceOwner_.allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(deviceOwner_.allocator);
        deviceOwner_.allocator = VK_NULL_HANDLE;
    }
    if (deviceOwner_.device != VK_NULL_HANDLE) {
        vkDestroyDevice(deviceOwner_.device, nullptr);
        deviceOwner_.device = VK_NULL_HANDLE;
    }
    if (swapchainOwner_.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(deviceOwner_.instance, swapchainOwner_.surface, nullptr);
        swapchainOwner_.surface = VK_NULL_HANDLE;
    }
    if (deviceOwner_.debugMessenger != VK_NULL_HANDLE) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(deviceOwner_.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) {
            destroy(deviceOwner_.instance, deviceOwner_.debugMessenger, nullptr);
        }
        deviceOwner_.debugMessenger = VK_NULL_HANDLE;
    }
    if (deviceOwner_.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(deviceOwner_.instance, nullptr);
        deviceOwner_.instance = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::Impl::waitIdle() {
    if (deviceOwner_.device != VK_NULL_HANDLE) {
        checkVk(vkDeviceWaitIdle(deviceOwner_.device), "vkDeviceWaitIdle");
        for (FrameResources& frame : frameOwner_.frames) {
            validateGpuVisibility(frame);
        }
        const std::size_t lastFrameIndex =
            (frameOwner_.currentFrame + kMaxFramesInFlight - 1U) %
            kMaxFramesInFlight;
        const FrameResources& lastFrame = frameOwner_.frames[lastFrameIndex];
        if (indirectSceneDrawsEnabled_ &&
            lastFrame.completedGpuCullCountersValid) {
            stats_.sceneItemCount = lastFrame.submittedSceneItemCount;
            stats_.visibleItemCount = lastFrame.completedVisibleItemCount;
            stats_.culledItemCount =
                lastFrame.submittedSceneItemCount -
                std::min(lastFrame.submittedSceneItemCount,
                         lastFrame.completedVisibleItemCount);
            stats_.visibleClusterInstanceCount =
                lastFrame.completedVisibleClusterInstanceCount;
            stats_.testedClusterInstanceCount =
                lastFrame.completedTestedClusterInstanceCount;
            stats_.occludedClusterInstanceCount =
                lastFrame.completedOccludedClusterInstanceCount;
            stats_.sphereLodHighCount =
                lastFrame.completedSphereLodCounts[0];
            stats_.sphereLodMediumCount =
                lastFrame.completedSphereLodCounts[1];
            stats_.sphereLodLowCount =
                lastFrame.completedSphereLodCounts[2];
            stats_.sceneTriangleCount =
                lastFrame.completedSceneTriangleCount;
            stats_.triangleCount =
                stats_.sceneTriangleCount *
                    lastFrame.submittedScenePassCount +
                1ULL;
            stats_.gpuVisibilityValidated =
                config_.gpuVisibilityValidation;
        }
        if (frameOwner_.timestampsEnabled) {
            const std::uint32_t timestampFrameIndex =
                static_cast<std::uint32_t>(lastFrameIndex);
            readBackGpuTimestamp(timestampFrameIndex);
        }
        const std::uint64_t validationErrorCount =
            deviceOwner_.validationMessages.errorCount.load(std::memory_order_relaxed);
        if (config_.requireValidation && validationErrorCount > 0U) {
            throw std::runtime_error(
                "Strict Vulkan validation observed " + std::to_string(validationErrorCount) +
                " error message(s); inspect the Vulkan validation log above");
        }
    }
}

} // namespace ve
