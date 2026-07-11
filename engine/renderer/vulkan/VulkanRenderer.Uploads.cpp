#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VkCommandBuffer VulkanRenderer::Impl::beginOneShotUploadCommands(const VkCommandPool commandPool, const char* operationName) const {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    const std::string allocateOperation = std::string("vkAllocateCommandBuffers ") + operationName;
    checkVk(vkAllocateCommandBuffers(deviceOwner_.device, &allocInfo, &commandBuffer), allocateOperation.c_str());
    try {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        const std::string beginOperation = std::string("vkBeginCommandBuffer ") + operationName;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), beginOperation.c_str());
    } catch (...) {
        vkFreeCommandBuffers(deviceOwner_.device, commandPool, 1, &commandBuffer);
        throw;
    }
    return commandBuffer;
}

VkCommandBuffer VulkanRenderer::Impl::beginGraphicsUploadCommands() const {
    return beginOneShotUploadCommands(frameOwner_.graphicsCommandPool, "graphics upload");
}

void VulkanRenderer::Impl::submitGraphicsUpload(VkCommandBuffer commandBuffer, Buffer staging) {
    submitUploadBatch(deviceOwner_.graphicsQueue, frameOwner_.graphicsCommandPool, commandBuffer, "graphics upload", staging, false);
}

void VulkanRenderer::Impl::submitUploadBatch(const VkQueue queue,
                                       const VkCommandPool commandPool,
                                       const VkCommandBuffer commandBuffer,
                                       const char* operationName,
                                       Buffer staging,
                                       const bool signalSemaphore) {
    PendingUploadBatch upload{};
    upload.commandPool = commandPool;
    upload.commandBuffer = commandBuffer;
    upload.staging = staging;
    bool queued = false;
    try {
        const std::string endOperation = std::string("vkEndCommandBuffer ") + operationName;
        checkVk(vkEndCommandBuffer(commandBuffer), endOperation.c_str());
        setObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, handleToUint64(commandBuffer), std::string("One-Shot ") + operationName + " Command Buffer");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        const std::string fenceOperation = std::string("vkCreateFence ") + operationName;
        checkVk(vkCreateFence(deviceOwner_.device, &fenceInfo, nullptr, &upload.fence), fenceOperation.c_str());
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(upload.fence), std::string("One-Shot ") + operationName + " Fence");

        if (signalSemaphore) {
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            const std::string semaphoreOperation = std::string("vkCreateSemaphore ") + operationName;
            checkVk(vkCreateSemaphore(deviceOwner_.device, &semaphoreInfo, nullptr, &upload.signalSemaphore), semaphoreOperation.c_str());
            setObjectName(VK_OBJECT_TYPE_SEMAPHORE, handleToUint64(upload.signalSemaphore), std::string("One-Shot ") + operationName + " Semaphore");
        }

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        frameOwner_.pendingUploads.push_back(std::move(upload));
        queued = true;
        PendingUploadBatch& queuedUpload = frameOwner_.pendingUploads.back();
        if (queuedUpload.signalSemaphore != VK_NULL_HANDLE) {
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &queuedUpload.signalSemaphore;
        }
        const std::string submitOperation = std::string("vkQueueSubmit ") + operationName;
        checkVk(vkQueueSubmit(queue, 1, &submitInfo, queuedUpload.fence), submitOperation.c_str());
    } catch (...) {
        if (queued) {
            destroyPendingUpload(frameOwner_.pendingUploads.back());
            frameOwner_.pendingUploads.pop_back();
        } else {
            destroyPendingUpload(upload);
        }
        throw;
    }
}

void VulkanRenderer::Impl::retirePendingUploadResources(PendingUploadBatch& upload) {
    destroyBuffer(upload.staging);
    if (upload.fence != VK_NULL_HANDLE) {
        vkDestroyFence(deviceOwner_.device, upload.fence, nullptr);
        upload.fence = VK_NULL_HANDLE;
    }
    if (upload.commandBuffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(deviceOwner_.device, upload.commandPool, 1, &upload.commandBuffer);
        upload.commandBuffer = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::Impl::destroyPendingUpload(PendingUploadBatch& upload) {
    retirePendingUploadResources(upload);
    if (upload.signalSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(deviceOwner_.device, upload.signalSemaphore, nullptr);
        upload.signalSemaphore = VK_NULL_HANDLE;
    }
    upload.commandPool = VK_NULL_HANDLE;
}

void VulkanRenderer::Impl::retireCompletedUploads() {
    for (std::size_t index = 0; index < frameOwner_.pendingUploads.size();) {
        PendingUploadBatch& upload = frameOwner_.pendingUploads[index];
        if (upload.fence == VK_NULL_HANDLE) {
            if (upload.signalSemaphore == VK_NULL_HANDLE) {
                if (index + 1U < frameOwner_.pendingUploads.size()) {
                    frameOwner_.pendingUploads[index] = std::move(frameOwner_.pendingUploads.back());
                }
                frameOwner_.pendingUploads.pop_back();
                continue;
            }
            ++index;
            continue;
        }

        const VkResult status = vkGetFenceStatus(deviceOwner_.device, upload.fence);
        if (status == VK_NOT_READY) {
            ++index;
            continue;
        }
        checkVk(status, "vkGetFenceStatus upload");
        retirePendingUploadResources(upload);
        if (upload.signalSemaphore == VK_NULL_HANDLE) {
            if (index + 1U < frameOwner_.pendingUploads.size()) {
                frameOwner_.pendingUploads[index] = std::move(frameOwner_.pendingUploads.back());
            }
            frameOwner_.pendingUploads.pop_back();
            continue;
        }
        ++index;
    }
}

void VulkanRenderer::Impl::destroyFrameUploadWaitSemaphores(FrameResources& frame) {
    for (VkSemaphore& semaphore : frame.uploadWaitSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(deviceOwner_.device, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
        }
    }
    frame.uploadWaitSemaphores.clear();
}

void VulkanRenderer::Impl::collectPendingUploadWaitSemaphores(std::vector<VkSemaphore>& semaphores) const {
    semaphores.clear();
    semaphores.reserve(frameOwner_.pendingUploads.size());
    for (const PendingUploadBatch& upload : frameOwner_.pendingUploads) {
        if (upload.signalSemaphore == VK_NULL_HANDLE) {
            continue;
        }
        semaphores.push_back(upload.signalSemaphore);
    }
}

void VulkanRenderer::Impl::markUploadWaitSemaphoresQueued(FrameResources& frame) noexcept {
    vulkan_renderer_detail::queueReservedUploadWaitSemaphores(frameOwner_.pendingUploads, frame.uploadWaitSemaphores);
}

bool VulkanRenderer::Impl::formatSupportsLinearMipBlit(const VkFormat format) const {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(deviceOwner_.physicalDevice, format, &properties);
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


        mipWidth = std::max(1, mipWidth / 2);
        mipHeight = std::max(1, mipHeight / 2);
    }

    if (image.mipLevels > 1U) {
        transitionImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        0, image.mipLevels - 1U);
    }

    transitionImage(commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    image.mipLevels - 1U, 1);
    image.syncState = imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::SampledImage);
}

VkCommandBuffer VulkanRenderer::Impl::beginUploadCommands() const {
    return beginOneShotUploadCommands(frameOwner_.transferCommandPool, "transfer upload");
}

void VulkanRenderer::Impl::submitTransferUpload(VkCommandBuffer commandBuffer, Buffer staging) {
    const bool needsQueueSemaphore = deviceOwner_.transferQueue != deviceOwner_.graphicsQueue;
    submitUploadBatch(deviceOwner_.transferQueue, frameOwner_.transferCommandPool, commandBuffer, "transfer upload", staging, needsQueueSemaphore);
}

} // namespace ve
