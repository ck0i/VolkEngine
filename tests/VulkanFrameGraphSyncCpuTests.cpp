#include "renderer/vulkan/VulkanFrameGraphSync.hpp"

#include <cassert>
#include <stdexcept>

namespace {

template <typename F>
bool throwsRuntimeError(F&& function) {
    try {
        static_cast<void>(function());
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

} // namespace

int main() {
    using ve::FrameGraphAccess;
    using ve::FrameGraphUsage;

    const ve::VulkanImageSyncState colorWrite =
        ve::vulkanImageSyncState(FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment);
    assert(colorWrite.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    assert(colorWrite.stage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    assert(colorWrite.access == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const ve::VulkanImageSyncState depthRead =
        ve::vulkanImageSyncState(FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment);
    assert(depthRead.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    assert((depthRead.stage & VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT) != 0U);
    assert(depthRead.access == VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

    const ve::VulkanImageSyncState sampled =
        ve::vulkanImageSyncState(FrameGraphAccess::Read, FrameGraphUsage::SampledImage);
    assert(sampled.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(sampled.access == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const ve::VulkanImageSyncState transferSource =
        ve::vulkanImageSyncState(FrameGraphAccess::Read, FrameGraphUsage::TransferSource);
    assert(transferSource.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(transferSource.access == VK_ACCESS_2_TRANSFER_READ_BIT);

    const ve::VulkanImageSyncState transferDestination =
        ve::vulkanImageSyncState(FrameGraphAccess::Write, FrameGraphUsage::TransferDestination);
    assert(transferDestination.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(transferDestination.access == VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const ve::VulkanImageSyncState present =
        ve::vulkanImageSyncState(FrameGraphAccess::Read, FrameGraphUsage::Present);
    assert(present.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    assert(present.stage == VK_PIPELINE_STAGE_2_NONE);
    assert(present.access == VK_ACCESS_2_NONE);

    const ve::VulkanImageSyncState acquired = ve::vulkanAcquiredImageSyncState();
    assert(acquired.layout == VK_IMAGE_LAYOUT_UNDEFINED);
    assert(acquired.stage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    assert(acquired.access == VK_ACCESS_2_NONE);

    assert(ve::vulkanAttachmentStoreOp(
               FrameGraphAccess::Write, ve::FrameGraphAttachmentStore::Store) ==
           VK_ATTACHMENT_STORE_OP_STORE);
    assert(ve::vulkanAttachmentStoreOp(
               FrameGraphAccess::Write, ve::FrameGraphAttachmentStore::Discard) ==
           VK_ATTACHMENT_STORE_OP_DONT_CARE);
    assert(ve::vulkanAttachmentStoreOp(
               FrameGraphAccess::Read, ve::FrameGraphAttachmentStore::Discard) ==
           VK_ATTACHMENT_STORE_OP_NONE);

    assert(throwsRuntimeError([] {
        return ve::vulkanImageSyncState(FrameGraphAccess::Read, FrameGraphUsage::UniformBuffer);
    }));
    assert(throwsRuntimeError([] {
        return ve::vulkanImageSyncState(FrameGraphAccess::Write, FrameGraphUsage::SampledImage);
    }));

    const ve::VulkanBufferSyncState uniform =
        ve::vulkanBufferSyncState(FrameGraphAccess::Read, FrameGraphUsage::UniformBuffer);
    assert(uniform.access == VK_ACCESS_2_UNIFORM_READ_BIT);
    const ve::VulkanBufferSyncState storageWrite =
        ve::vulkanBufferSyncState(FrameGraphAccess::Write, FrameGraphUsage::StorageBuffer);
    assert(storageWrite.access == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const ve::VulkanBufferSyncState indirect =
        ve::vulkanBufferSyncState(FrameGraphAccess::Read, FrameGraphUsage::IndirectBuffer);
    assert(indirect.stage == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
    assert(indirect.access == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    const ve::VulkanBufferSyncState bufferSource =
        ve::vulkanBufferSyncState(FrameGraphAccess::Read, FrameGraphUsage::TransferSource);
    assert(bufferSource.access == VK_ACCESS_2_TRANSFER_READ_BIT);
    const ve::VulkanBufferSyncState bufferDestination =
        ve::vulkanBufferSyncState(FrameGraphAccess::Write, FrameGraphUsage::TransferDestination);
    assert(bufferDestination.access == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const ve::VulkanBufferSyncState hostRead =
        ve::vulkanBufferSyncState(FrameGraphAccess::Read, FrameGraphUsage::HostRead);
    assert(hostRead.stage == VK_PIPELINE_STAGE_2_HOST_BIT);
    assert(hostRead.access == VK_ACCESS_2_HOST_READ_BIT);
    assert(throwsRuntimeError([] {
        return ve::vulkanImageSyncState(FrameGraphAccess::Read, FrameGraphUsage::HostRead);
    }));

    assert(throwsRuntimeError([] {
        return ve::vulkanBufferSyncState(FrameGraphAccess::Read, FrameGraphUsage::ColorAttachment);
    }));
    assert(throwsRuntimeError([] {
        return ve::vulkanBufferSyncState(FrameGraphAccess::Write, FrameGraphUsage::UniformBuffer);
    }));
    assert(throwsRuntimeError([] {
        return ve::vulkanBufferSyncState(FrameGraphAccess::Write, FrameGraphUsage::IndirectBuffer);
    }));
    return 0;
}
