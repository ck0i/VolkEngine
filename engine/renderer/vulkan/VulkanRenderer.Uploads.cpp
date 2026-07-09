#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VkCommandBuffer VulkanRenderer::Impl::beginGraphicsUploadCommands() const {
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

void VulkanRenderer::Impl::submitGraphicsUpload(VkCommandBuffer commandBuffer, std::vector<Buffer> stagingBuffers) {
    submitUploadBatch(graphicsQueue_, graphicsCommandPool_, commandBuffer, "graphics upload", std::move(stagingBuffers), false);
}

void VulkanRenderer::Impl::submitUploadBatch(const VkQueue queue,
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

void VulkanRenderer::Impl::retirePendingUploadResources(PendingUploadBatch& upload) {
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

void VulkanRenderer::Impl::destroyPendingUpload(PendingUploadBatch& upload) {
    retirePendingUploadResources(upload);
    if (upload.signalSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, upload.signalSemaphore, nullptr);
        upload.signalSemaphore = VK_NULL_HANDLE;
    }
    upload.commandPool = VK_NULL_HANDLE;
}

void VulkanRenderer::Impl::retireCompletedUploads() {
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

void VulkanRenderer::Impl::destroyFrameUploadWaitSemaphores(FrameResources& frame) {
    for (VkSemaphore& semaphore : frame.uploadWaitSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
        }
    }
    frame.uploadWaitSemaphores.clear();
}

void VulkanRenderer::Impl::collectPendingUploadWaitSemaphores(std::vector<VkSemaphore>& semaphores) const {
    semaphores.clear();
    semaphores.reserve(pendingUploads_.size());
    for (const PendingUploadBatch& upload : pendingUploads_) {
        if (upload.signalSemaphore == VK_NULL_HANDLE) {
            continue;
        }
        semaphores.push_back(upload.signalSemaphore);
    }
}

void VulkanRenderer::Impl::markUploadWaitSemaphoresQueued(FrameResources& frame,
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

bool VulkanRenderer::Impl::formatSupportsLinearMipBlit(const VkFormat format) const {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
    constexpr VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                      VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (properties.optimalTilingFeatures & requiredFeatures) == requiredFeatures;
}

void VulkanRenderer::Impl::generateMipmaps(VkCommandBuffer commandBuffer, ImageResource& image) const {
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

VkCommandBuffer VulkanRenderer::Impl::beginUploadCommands() const {
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

void VulkanRenderer::Impl::submitTransferUpload(VkCommandBuffer commandBuffer, std::vector<Buffer> stagingBuffers) {
    const bool needsQueueSemaphore = transferQueue_ != graphicsQueue_;
    submitUploadBatch(transferQueue_, transferCommandPool_, commandBuffer, "transfer upload", std::move(stagingBuffers), needsQueueSemaphore);
}

} // namespace ve
