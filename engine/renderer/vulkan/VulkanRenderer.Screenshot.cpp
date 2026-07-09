#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

void VulkanRenderer::Impl::requestScreenshot(std::filesystem::path path) {
    const std::scoped_lock lock{screenshotRequestMutex_};
    screenshotPath_ = std::move(path);
    screenshotPending_ = true;
}

bool VulkanRenderer::Impl::screenshotFormatSupported() const {
    return swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapchainFormat_ == VK_FORMAT_R8G8B8A8_UNORM;
}

void VulkanRenderer::Impl::recordScreenshotCopy(const VkCommandBuffer commandBuffer, const std::uint32_t imageIndex, const Buffer& readback) {
    transitionImageTracked(commandBuffer, swapchainImages_[imageIndex], swapchainStates_[imageIndex],
                           imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::TransferSource),
                           VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {swapchainExtent_.width, swapchainExtent_.height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &region);
}

void VulkanRenderer::Impl::writeScreenshotPpm(const Buffer& readback, const VkExtent2D extent, const VkFormat format, const std::filesystem::path& path) const {
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM) {
        throw std::runtime_error("Screenshot capture only supports BGRA8/RGBA8 swapchain formats");
    }

    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    ScopedVmaMap mappedReadback{allocator_, readback.allocation, "vmaMapMemory screenshot readback"};
    const auto* src = static_cast<const std::uint8_t*>(mappedReadback.get());

    std::filesystem::path tempPath = path;
    tempPath += ".tmp";

    try {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("Failed to open screenshot temp file: " + tempPath.string());
        }

        file << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
        std::vector<std::uint8_t> row(static_cast<std::size_t>(extent.width) * 3U);
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            const auto* srcRow = src + (static_cast<std::size_t>(y) * extent.width * 4U);
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                const std::size_t srcOffset = static_cast<std::size_t>(x) * 4U;
                const std::size_t dstOffset = static_cast<std::size_t>(x) * 3U;
                if (format == VK_FORMAT_B8G8R8A8_UNORM) {
                    row[dstOffset + 0U] = srcRow[srcOffset + 2U];
                    row[dstOffset + 1U] = srcRow[srcOffset + 1U];
                    row[dstOffset + 2U] = srcRow[srcOffset + 0U];
                } else {
                    row[dstOffset + 0U] = srcRow[srcOffset + 0U];
                    row[dstOffset + 1U] = srcRow[srcOffset + 1U];
                    row[dstOffset + 2U] = srcRow[srcOffset + 2U];
                }
            }
            file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
        file.close();
        if (!file) {
            throw std::runtime_error("Failed to write screenshot temp file: " + tempPath.string());
        }
    } catch (...) {
        std::error_code removeError;
        std::filesystem::remove(tempPath, removeError);
        throw;
    }

    std::error_code publishError;
    std::filesystem::rename(tempPath, path, publishError);
    if (!publishError) {
        return;
    }

    std::error_code existsError;
    const bool targetExists = std::filesystem::exists(path, existsError);
    if (!existsError && targetExists) {
        std::filesystem::path backupPath;
        bool backupCreated = false;
        const auto backupSeed = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint32_t attempt = 0; attempt < 16U && !backupCreated; ++attempt) {
            backupPath = path;
            backupPath += ".bak.";
            backupPath += std::to_string(backupSeed);
            backupPath += ".";
            backupPath += std::to_string(attempt);
            std::error_code backupExistsError;
            const bool backupExists = std::filesystem::exists(backupPath, backupExistsError);
            if (backupExistsError || backupExists) {
                continue;
            }

            std::error_code backupError;
            std::filesystem::rename(path, backupPath, backupError);
            backupCreated = !backupError;
        }

        if (backupCreated) {
            publishError.clear();
            std::filesystem::rename(tempPath, path, publishError);
            if (!publishError) {
                std::error_code removeBackupError;
                std::filesystem::remove(backupPath, removeBackupError);
                return;
            }

            std::error_code restoreError;
            std::filesystem::rename(backupPath, path, restoreError);
            if (restoreError) {
                throw std::runtime_error("Failed to publish screenshot file: " + path.string()
                                         + " (old screenshot kept at " + backupPath.string()
                                         + ", temp kept at " + tempPath.string() + "): " + publishError.message());
            }
        }
    }

    throw std::runtime_error("Failed to publish screenshot file: " + path.string()
                             + " (temp kept at " + tempPath.string() + "): " + publishError.message());
}

} // namespace ve
