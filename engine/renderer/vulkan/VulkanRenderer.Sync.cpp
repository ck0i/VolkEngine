#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VulkanRenderer::Impl::ImageSyncState VulkanRenderer::Impl::imageSyncStateFor(const FrameGraphAccess access, const FrameGraphUsage usage) {
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
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Present frame-graph usage must be read-only");
        }
        return finalImageSyncStateFor(usage);
    }
    throw std::runtime_error("Unknown frame-graph image usage");
}

VulkanRenderer::Impl::ImageSyncState VulkanRenderer::Impl::finalImageSyncStateFor(const FrameGraphUsage usage) {
    if (usage != FrameGraphUsage::Present) {
        throw std::runtime_error("Unsupported frame-graph final image usage");
    }
    return ImageSyncState{VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
}

VulkanRenderer::Impl::FrameImageSyncSnapshot VulkanRenderer::Impl::captureFrameImageSyncState(const std::uint32_t imageIndex) const {
    return FrameImageSyncSnapshot{
        depth_.syncState,
        hdr_.syncState,
        swapchainStates_[imageIndex],
    };
}

void VulkanRenderer::Impl::restoreFrameImageSyncState(const std::uint32_t imageIndex, const FrameImageSyncSnapshot& snapshot) {
    depth_.syncState = snapshot.depth;
    hdr_.syncState = snapshot.hdr;
    swapchainStates_[imageIndex] = snapshot.swapchain;
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
                                            const std::uint32_t baseMipLevel, const std::uint32_t levelCount) const {
    if (syncState.layout == newState.layout && syncState.stage == newState.stage && syncState.access == newState.access) {
        return;
    }
    transitionImage(cmd, image, syncState.layout, newState.layout, aspect,
                    syncState.stage, syncState.access, newState.stage, newState.access,
                    baseMipLevel, levelCount);
    syncState = newState;
}

} // namespace ve
