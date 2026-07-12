#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VulkanRenderer::Impl::SwapchainSupport VulkanRenderer::Impl::querySwapchainSupport(VkPhysicalDevice device) const {
    SwapchainSupport support{};
    checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, swapchainOwner_.surface, &support.capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    std::uint32_t formatCount = 0;
    checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device, swapchainOwner_.surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR count");
    support.formats.resize(formatCount);
    if (formatCount > 0U) {
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device, swapchainOwner_.surface, &formatCount, support.formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR data");
    }

    std::uint32_t presentModeCount = 0;
    checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, swapchainOwner_.surface, &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR count");
    support.presentModes.resize(presentModeCount);
    if (presentModeCount > 0U) {
        checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, swapchainOwner_.surface, &presentModeCount, support.presentModes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR data");
    }
    return support;
}

VkSurfaceFormatKHR VulkanRenderer::Impl::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    // The tonemap shader normally writes sRGB-encoded LDR values. Prefer UNORM
    // swapchain formats so Vulkan does not apply a second conversion on color writes.
    for (const VkSurfaceFormatKHR& format : formats) {
        if (isUnormSwapchainFormat(format.format) && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (isUnormSwapchainFormat(format.format)) {
            return format;
        }
    }
    const VkSurfaceFormatKHR fallback = formats.front();
    if (isSrgbSwapchainFormat(fallback.format)) {
        logger()->warn("Selected sRGB swapchain format {}; disabling shader-side sRGB OETF to avoid double encoding",
                       static_cast<int>(fallback.format));
    }
    return fallback;
}

VkPresentModeKHR VulkanRenderer::Impl::choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) const {
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

VkExtent2D VulkanRenderer::Impl::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent = window_.framebufferExtent();
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

void VulkanRenderer::Impl::createSwapchain() {
    const SwapchainSupport support = querySwapchainSupport(deviceOwner_.physicalDevice);
    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    swapchainOwner_.presentMode = choosePresentMode(support.presentModes);
    const VkExtent2D extent = chooseExtent(support.capabilities);

    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = swapchainOwner_.surface;
    createInfo.minImageCount = clampImageCount(support.capabilities);
    swapchainOwner_.minimumImageCount = createInfo.minImageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    readback_.setSwapchainTransferSourceSupported(
        (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U);
    if (readback_.swapchainTransferSourceSupported()) {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    const std::array<std::uint32_t, 2> queueFamilyIndices{deviceOwner_.queueFamilies.graphics.value(), deviceOwner_.queueFamilies.present.value()};
    if (deviceOwner_.queueFamilies.graphics != deviceOwner_.queueFamilies.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilyIndices.size());
        createInfo.pQueueFamilyIndices = queueFamilyIndices.data();
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = swapchainOwner_.presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    checkVk(vkCreateSwapchainKHR(deviceOwner_.device, &createInfo, nullptr, &swapchainOwner_.handle), "vkCreateSwapchainKHR");

    setObjectName(VK_OBJECT_TYPE_SWAPCHAIN_KHR, handleToUint64(swapchainOwner_.handle), "Main Swapchain");
    std::uint32_t imageCount = 0;
    checkVk(vkGetSwapchainImagesKHR(deviceOwner_.device, swapchainOwner_.handle, &imageCount, nullptr), "vkGetSwapchainImagesKHR count");
    swapchainOwner_.images.resize(imageCount);
    checkVk(vkGetSwapchainImagesKHR(deviceOwner_.device, swapchainOwner_.handle, &imageCount, swapchainOwner_.images.data()), "vkGetSwapchainImagesKHR data");
    swapchainOwner_.format = surfaceFormat.format;
    swapchainOwner_.extent = extent;
    swapchainOwner_.imageStates.assign(imageCount, vulkanAcquiredImageSyncState());


    swapchainOwner_.renderFinishedSemaphores.resize(imageCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (std::size_t i = 0; i < swapchainOwner_.renderFinishedSemaphores.size(); ++i) {
        checkVk(vkCreateSemaphore(deviceOwner_.device, &semaphoreInfo, nullptr, &swapchainOwner_.renderFinishedSemaphores[i]), "vkCreateSemaphore swapchain renderFinished");
        setObjectName(VK_OBJECT_TYPE_SEMAPHORE, handleToUint64(swapchainOwner_.renderFinishedSemaphores[i]),
                      "Swapchain Image " + std::to_string(i) + " Render Finished Semaphore");
    }
    logger()->info("Created swapchain {}x{} with {} images ({})", extent.width, extent.height, imageCount, presentModeName(swapchainOwner_.presentMode));
}

void VulkanRenderer::Impl::createImageViews() {
    swapchainOwner_.imageViews.resize(swapchainOwner_.images.size());
    swapchainOwner_.resourceIds.resize(swapchainOwner_.images.size(), GpuResourceRegistry::kInvalidId);
    for (std::size_t i = 0; i < swapchainOwner_.images.size(); ++i) {
        swapchainOwner_.imageViews[i] = createImageView(swapchainOwner_.images[i], swapchainOwner_.format, VK_IMAGE_ASPECT_COLOR_BIT);
        const std::string index = std::to_string(i);
        setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(swapchainOwner_.images[i]), "Swapchain Image " + index);
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(swapchainOwner_.imageViews[i]), "Swapchain Image View " + index);
        const std::uint64_t swapchainBytes = imageByteEstimate(swapchainOwner_.extent, swapchainOwner_.format);
        swapchainOwner_.resourceIds[i] = resourceOwner_.registry.registerResource(GpuResourceKind::Image, "Swapchain Image", swapchainBytes, true);
    }
}

VkFormat VulkanRenderer::Impl::findDepthFormat() const {
    constexpr std::array<VkFormat, 3> candidates{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(deviceOwner_.physicalDevice, format, &properties);
        constexpr VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & required) == required) {
            return format;
        }
    }
    throw std::runtime_error("No supported depth format found");
}
VkFormat VulkanRenderer::Impl::findShadowDepthFormat() const {
    constexpr std::array<VkFormat, 4> candidates{
        VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    constexpr VkFormatFeatureFlags2 required =
        VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
    for (const VkFormat format : candidates) {
        VkFormatProperties3 properties3{
            VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 properties2{
            VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        properties2.pNext = &properties3;
        vkGetPhysicalDeviceFormatProperties2(
            deviceOwner_.physicalDevice, format, &properties2);
        if ((properties3.optimalTilingFeatures & required) == required) {
            return format;
        }
    }
    throw std::runtime_error("No supported shadow depth format found");
}
VkFormat VulkanRenderer::Impl::findHdrFormat() const {
    constexpr std::array<VkFormat, 2> candidates{
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_R16G16B16A16_SFLOAT};
    constexpr VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    for (const VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(
            deviceOwner_.physicalDevice, format, &properties);
        if ((properties.optimalTilingFeatures & required) == required) {
            return format;
        }
    }
    throw std::runtime_error("No supported HDR color format found");
}



void VulkanRenderer::Impl::realizeFrameGraphResources() {
    ImageResource replacementDepth;
    ImageResource replacementHdr;
    ImageResource replacementDepthPyramid;
    std::vector<VkImageView> replacementDepthPyramidMipViews;
    try {
        replacementDepth = createImage(
            swapchainOwner_.extent, findDepthFormat(),
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(replacementDepth.image), "Depth Image");
        vmaSetAllocationName(deviceOwner_.allocator, replacementDepth.allocation, "Depth Image Allocation");
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(replacementDepth.view), "Depth Image View");
        replacementDepth.resourceId = resourceOwner_.registry.registerResource(
            GpuResourceKind::Image, "Depth Image",
            imageByteEstimate(replacementDepth.extent, replacementDepth.format));

        replacementHdr = createImage(
            swapchainOwner_.extent, findHdrFormat(),
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
        setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(replacementHdr.image), "HDR Color Image");
        vmaSetAllocationName(deviceOwner_.allocator, replacementHdr.allocation, "HDR Color Image Allocation");
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(replacementHdr.view), "HDR Color Image View");
        replacementHdr.resourceId = resourceOwner_.registry.registerResource(
            GpuResourceKind::Image, "HDR Color Image",
            imageByteEstimate(replacementHdr.extent, replacementHdr.format));
        const VkExtent2D depthPyramidExtent{
            swapchainOwner_.extent.width / 2U +
                swapchainOwner_.extent.width % 2U,
            swapchainOwner_.extent.height / 2U +
                swapchainOwner_.extent.height % 2U};
        const std::uint32_t depthPyramidMipLevels =
            1U + static_cast<std::uint32_t>(std::floor(std::log2(
                     static_cast<double>(std::max(
                         depthPyramidExtent.width,
                         depthPyramidExtent.height)))));
        if (depthPyramidMipLevels > kMaxDepthPyramidMipLevels) {
            throw std::runtime_error(
                "Depth pyramid mip count exceeds renderer limit");
        }
        VkFormatProperties extremaProperties{};
        vkGetPhysicalDeviceFormatProperties(
            deviceOwner_.physicalDevice, VK_FORMAT_R32G32_SFLOAT,
            &extremaProperties);
        constexpr VkFormatFeatureFlags kExtremaFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        resourceOwner_.depthPyramidExtremaEnabled =
            (extremaProperties.optimalTilingFeatures & kExtremaFeatures) ==
            kExtremaFeatures;
        const VkFormat depthPyramidFormat =
            resourceOwner_.depthPyramidExtremaEnabled
                ? VK_FORMAT_R32G32_SFLOAT
                : VK_FORMAT_R32_SFLOAT;
        replacementDepthPyramid = createImage(
            depthPyramidExtent, depthPyramidFormat,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, depthPyramidMipLevels);
        setObjectName(VK_OBJECT_TYPE_IMAGE,
                      handleToUint64(replacementDepthPyramid.image),
                      "Depth Pyramid Image");
        vmaSetAllocationName(deviceOwner_.allocator,
                             replacementDepthPyramid.allocation,
                             "Depth Pyramid Allocation");
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW,
                      handleToUint64(replacementDepthPyramid.view),
                      "Depth Pyramid Full View");
        replacementDepthPyramidMipViews.reserve(depthPyramidMipLevels);
        for (std::uint32_t mip = 0; mip < depthPyramidMipLevels; ++mip) {
            VkImageViewCreateInfo viewInfo{
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = replacementDepthPyramid.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = depthPyramidFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = mip;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            checkVk(vkCreateImageView(deviceOwner_.device, &viewInfo, nullptr,
                                      &view),
                    "vkCreateImageView depth pyramid mip");
            replacementDepthPyramidMipViews.push_back(view);
            setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(view),
                          "Depth Pyramid Mip " + std::to_string(mip));
        }
        replacementDepthPyramid.resourceId =
            resourceOwner_.registry.registerResource(
                GpuResourceKind::Image, "Depth Pyramid Image",
                replacementDepthPyramid.allocationBytes);
    } catch (...) {
        for (VkImageView view : replacementDepthPyramidMipViews) {
            vkDestroyImageView(deviceOwner_.device, view, nullptr);
        }
        replacementDepthPyramidMipViews.clear();
        destroyImage(replacementDepthPyramid);
        destroyImage(replacementHdr);
        destroyImage(replacementDepth);
        throw;
    }

    ImageResource oldDepth = takeImage(resourceOwner_.depth);
    ImageResource oldHdr = takeImage(resourceOwner_.hdr);
    ImageResource oldDepthPyramid =
        takeImage(resourceOwner_.depthPyramid);
    std::vector<VkImageView> oldDepthPyramidMipViews =
        std::move(resourceOwner_.depthPyramidMipViews);
    resourceOwner_.depth = takeImage(replacementDepth);
    resourceOwner_.hdr = takeImage(replacementHdr);
    resourceOwner_.depthPyramid = takeImage(replacementDepthPyramid);
    resourceOwner_.depthPyramidMipViews =
        std::move(replacementDepthPyramidMipViews);
    resourceOwner_.depthPyramidValid = false;
    for (VkImageView view : oldDepthPyramidMipViews) {
        vkDestroyImageView(deviceOwner_.device, view, nullptr);
    }
    destroyImage(oldDepthPyramid);
    destroyImage(oldHdr);
    destroyImage(oldDepth);
}

void VulkanRenderer::Impl::cleanupSwapchain() {
    for (VkImageView view : resourceOwner_.depthPyramidMipViews) {
        vkDestroyImageView(deviceOwner_.device, view, nullptr);
    }
    resourceOwner_.depthPyramidMipViews.clear();
    destroyImage(resourceOwner_.depthPyramid);
    resourceOwner_.depthPyramidValid = false;
    destroyImage(resourceOwner_.hdr);
    destroyImage(resourceOwner_.depth);
    for (VkImageView view : swapchainOwner_.imageViews) {
        vkDestroyImageView(deviceOwner_.device, view, nullptr);
    }
    for (const std::uint32_t resourceId : swapchainOwner_.resourceIds) {
        resourceOwner_.registry.unregisterResource(resourceId);
    }
    swapchainOwner_.resourceIds.clear();
    swapchainOwner_.imageViews.clear();
    for (VkSemaphore semaphore : swapchainOwner_.renderFinishedSemaphores) {
        vkDestroySemaphore(deviceOwner_.device, semaphore, nullptr);
    }
    swapchainOwner_.renderFinishedSemaphores.clear();
    swapchainOwner_.images.clear();
    swapchainOwner_.imageStates.clear();
    if (swapchainOwner_.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(deviceOwner_.device, swapchainOwner_.handle, nullptr);
        swapchainOwner_.handle = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::Impl::recreateSwapchain() {
    VkExtent2D extent = window_.framebufferExtent();
    while ((extent.width == 0U || extent.height == 0U) && !window_.shouldClose()) {
        window_.waitEvents();
        extent = window_.framebufferExtent();
    }
    if (extent.width == 0U || extent.height == 0U) {
        return;
    }

    const VkFormat previousSwapchainFormat = swapchainOwner_.format;
    const VkFormat previousHdrFormat = resourceOwner_.hdr.format;
    const VkFormat previousDepthFormat = resourceOwner_.depth.format;
    const bool hadImGui = imguiOwner_.initialized;

    for (FrameResources& frame : frameOwner_.frames) {
        if (frame.inFlight != VK_NULL_HANDLE) {
            checkVk(vkWaitForFences(deviceOwner_.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
                    "vkWaitForFences swapchain recreation");
        }
    }
    checkVk(vkQueueWaitIdle(deviceOwner_.presentQueue), "vkQueueWaitIdle swapchain recreation");
    if (hadImGui) {
        shutdownImGui();
    }
    cleanupSwapchain();
    createSwapchain();
    createImageViews();
    createFrameGraph(true);
    realizeFrameGraphResources();
    if (hadImGui) {
        createImGui();
    }
    updateDepthPyramidDescriptors();

    const bool pipelineFormatsChanged = previousSwapchainFormat != swapchainOwner_.format || previousHdrFormat != resourceOwner_.hdr.format || previousDepthFormat != resourceOwner_.depth.format;
    if (pipelineFormatsChanged || pipelineOwner_.depthPrepass == VK_NULL_HANDLE || pipelineOwner_.scene == VK_NULL_HANDLE || pipelineOwner_.sceneNoPrepass == VK_NULL_HANDLE || pipelineOwner_.tonemap == VK_NULL_HANDLE) {
        PipelineSet oldPipelines = detachActivePipelineSet();
        destroyPipelineSet(oldPipelines);
        createPipelines();
    }
    createTonemapDescriptorSet();
    const GpuResourceRegistry::Stats resourceStats = resourceOwner_.registry.stats();
    logger()->info("Recreated swapchain; tracked GPU resources: {} live ({} buffers, {} images, {} imported), {:.2f} MiB estimated (buffers {:.2f}, owned images {:.2f}, imported images {:.2f})",
                   resourceStats.liveResources, resourceStats.buffers, resourceStats.images,
                   resourceStats.importedImages, bytesToMiB(resourceStats.bytes), bytesToMiB(resourceStats.bufferBytes),
                   bytesToMiB(resourceStats.ownedImageBytes), bytesToMiB(resourceStats.importedImageBytes));
}

} // namespace ve
