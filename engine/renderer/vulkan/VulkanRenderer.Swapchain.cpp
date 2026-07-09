#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {
namespace {

[[nodiscard]] bool isSrgbSwapchainFormat(const VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isUnormSwapchainFormat(const VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

} // namespace

VulkanRenderer::Impl::SwapchainSupport VulkanRenderer::Impl::querySwapchainSupport(VkPhysicalDevice device) const {
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

void VulkanRenderer::Impl::createImageViews() {
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

VkFormat VulkanRenderer::Impl::findDepthFormat() const {
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

void VulkanRenderer::Impl::createDepthResources() {
    depth_ = createImage(swapchainExtent_, findDepthFormat(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(depth_.image), "Depth Image");
    vmaSetAllocationName(allocator_, depth_.allocation, "Depth Image Allocation");
    setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(depth_.view), "Depth Image View");
    const std::uint64_t depthBytes = imageByteEstimate(depth_.extent, depth_.format);
    depth_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Image, "Depth Image", depthBytes);
}

void VulkanRenderer::Impl::createHdrResources() {
    hdr_ = createImage(swapchainExtent_, VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(hdr_.image), "HDR Color Image");
    vmaSetAllocationName(allocator_, hdr_.allocation, "HDR Color Image Allocation");
    setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(hdr_.view), "HDR Color Image View");
    const std::uint64_t hdrBytes = imageByteEstimate(hdr_.extent, hdr_.format);
    hdr_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Image, "HDR Color Image", hdrBytes);
}

void VulkanRenderer::Impl::cleanupSwapchain() {
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

void VulkanRenderer::Impl::recreateSwapchain() {
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
