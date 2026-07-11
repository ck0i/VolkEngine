#pragma once

#include "renderer/FrameGraph.hpp"

#include <stdexcept>
#include <vulkan/vulkan.h>

namespace ve {

struct VulkanImageSyncState {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

[[nodiscard]] inline constexpr VulkanImageSyncState vulkanAcquiredImageSyncState() noexcept {
    return {
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_NONE};
}

[[nodiscard]] inline constexpr VkAttachmentStoreOp vulkanAttachmentStoreOp(
    const FrameGraphAccess access, const FrameGraphAttachmentStore store) noexcept {
    if (store == FrameGraphAttachmentStore::Store) {
        return VK_ATTACHMENT_STORE_OP_STORE;
    }
    return access == FrameGraphAccess::Read
        ? VK_ATTACHMENT_STORE_OP_NONE
        : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

struct VulkanBufferSyncState {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

[[nodiscard]] inline VulkanImageSyncState vulkanImageSyncState(
    const FrameGraphAccess access, const FrameGraphUsage usage) {
    switch (usage) {
    case FrameGraphUsage::ColorAttachment:
        return {
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            access == FrameGraphAccess::Write
                ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT};
    case FrameGraphUsage::DepthAttachment:
        return {
            access == FrameGraphAccess::Read
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            access == FrameGraphAccess::Write
                ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
    case FrameGraphUsage::SampledImage:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Sampled image frame-graph usage must be read-only");
        }
        return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case FrameGraphUsage::TransferSource:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Transfer-source image usage must be read-only");
        }
        return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT};
    case FrameGraphUsage::TransferDestination:
        if (access != FrameGraphAccess::Write) {
            throw std::runtime_error("Transfer-destination image usage must be write-only");
        }
        return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case FrameGraphUsage::Present:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Present frame-graph usage must be read-only");
        }
        return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE};
    case FrameGraphUsage::UniformBuffer:
    case FrameGraphUsage::StorageBuffer:
    case FrameGraphUsage::IndirectBuffer:
    case FrameGraphUsage::HostRead:
        throw std::runtime_error("Buffer-only frame-graph usage cannot map to an image state");
    }
    throw std::runtime_error("Unknown frame-graph image usage");
}

[[nodiscard]] inline VulkanBufferSyncState vulkanBufferSyncState(
    const FrameGraphAccess access, const FrameGraphUsage usage) {
    switch (usage) {
    case FrameGraphUsage::UniformBuffer:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Uniform-buffer usage must be read-only");
        }
        return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_UNIFORM_READ_BIT};
    case FrameGraphUsage::StorageBuffer:
        return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                access == FrameGraphAccess::Write
                    ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    : VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case FrameGraphUsage::IndirectBuffer:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Indirect-buffer usage must be read-only");
        }
        return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    case FrameGraphUsage::TransferSource:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Transfer-source buffer usage must be read-only");
        }
        return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    case FrameGraphUsage::TransferDestination:
        if (access != FrameGraphAccess::Write) {
            throw std::runtime_error("Transfer-destination buffer usage must be write-only");
        }
        return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case FrameGraphUsage::HostRead:
        if (access != FrameGraphAccess::Read) {
            throw std::runtime_error("Host-read buffer usage must be read-only");
        }
        return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    case FrameGraphUsage::ColorAttachment:
    case FrameGraphUsage::DepthAttachment:
    case FrameGraphUsage::SampledImage:
    case FrameGraphUsage::Present:
        throw std::runtime_error("Image-only frame-graph usage cannot map to a buffer state");
    }
    throw std::runtime_error("Unknown frame-graph buffer usage");
}

} // namespace ve
