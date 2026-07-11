#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VulkanRenderer::Impl::ImageSyncState VulkanRenderer::Impl::imageSyncStateFor(
    const FrameGraphAccess access, const FrameGraphUsage usage) {
    return vulkanImageSyncState(access, usage);
}

VulkanRenderer::Impl::ImageSyncState VulkanRenderer::Impl::finalImageSyncStateFor(
    const FrameGraphUsage usage) {
    if (usage != FrameGraphUsage::Present) {
        throw std::runtime_error("Unsupported frame-graph final image usage");
    }
    return vulkanImageSyncState(FrameGraphAccess::Read, usage);
}

VulkanRenderer::Impl::FrameImageSyncSnapshot VulkanRenderer::Impl::captureFrameImageSyncState(const std::uint32_t imageIndex) const {
    return FrameImageSyncSnapshot{
        resourceOwner_.depth.syncState,
        resourceOwner_.hdr.syncState,
        swapchainOwner_.imageStates[imageIndex],
        readback_.buffer().syncState,
    };
}

void VulkanRenderer::Impl::restoreFrameImageSyncState(const std::uint32_t imageIndex, const FrameImageSyncSnapshot& snapshot) {
    resourceOwner_.depth.syncState = snapshot.depth;
    resourceOwner_.hdr.syncState = snapshot.hdr;
    swapchainOwner_.imageStates[imageIndex] = snapshot.swapchain;
    readback_.buffer().syncState = snapshot.screenshotReadback;
}

void VulkanRenderer::Impl::transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect,
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

void VulkanRenderer::Impl::transitionImageTracked(VkCommandBuffer cmd, VkImage image, ImageSyncState& syncState, ImageSyncState newState, VkImageAspectFlags aspect,
                                            const std::uint32_t baseMipLevel, const std::uint32_t levelCount, const bool forceMemoryDependency) const {
    if (!imageSyncStateRequiresBarrier(syncState.layout, syncState.stage, syncState.access,
                                       newState.layout, newState.stage, newState.access, forceMemoryDependency)) {
        return;
    }
    transitionImage(cmd, image, syncState.layout, newState.layout, aspect,
                    syncState.stage, syncState.access, newState.stage, newState.access,
                    baseMipLevel, levelCount);
    syncState = newState;
}

} // namespace ve
