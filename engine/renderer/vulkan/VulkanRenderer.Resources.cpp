#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {
namespace {

bool hasNonOpaqueAlpha(const LoadedImageRgba8& image) noexcept {
    for (std::size_t offset = 3; offset < image.pixels.size(); offset += 4U) {
        if (image.pixels[offset] != 255U) {
            return true;
        }
    }
    return false;
}

} // namespace

VkImageView VulkanRenderer::Impl::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, const std::uint32_t mipLevels) const {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &view), "vkCreateImageView");
    return view;
}

VulkanRenderer::Impl::ImageResource VulkanRenderer::Impl::createImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspectFlags, const std::uint32_t mipLevels) {
    ImageResource resource{};
    resource.format = format;
    resource.extent = extent;
    resource.mipLevels = mipLevels;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    checkVk(vmaCreateImage(allocator_, &imageInfo, &allocationInfo, &resource.image, &resource.allocation, nullptr), "vmaCreateImage");

    resource.view = createImageView(resource.image, format, aspectFlags, mipLevels);
    return resource;
}

void VulkanRenderer::Impl::destroyImage(ImageResource& image) {
    resourceRegistry_.unregisterResource(image.resourceId);
    image.resourceId = GpuResourceRegistry::kInvalidId;
    if (image.view != VK_NULL_HANDLE) { vkDestroyImageView(device_, image.view, nullptr); }
    if (image.image != VK_NULL_HANDLE) { vmaDestroyImage(allocator_, image.image, image.allocation); }
    image = {};
}

void VulkanRenderer::Impl::createTextureResources() {
    struct TextureUpload {
        std::filesystem::path path;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::string debugName;
        bool cpuNormalMipChain = false;
        std::uint32_t baseWidth = 0;
        std::uint32_t baseHeight = 0;
        std::vector<LoadedImageRgba8> mipLevels;
        std::vector<VkBufferImageCopy> copyRegions;
        bool gpuMipGeneration = false;
        ImageResource image;
    };

    std::array<TextureUpload, 2> uploads{{
        {.path = config_.assetDirectory / "textures" / "ground_albedo.png", .format = VK_FORMAT_R8G8B8A8_SRGB, .debugName = "Ground Albedo", .cpuNormalMipChain = false},
        {.path = config_.assetDirectory / "textures" / "ground_normal.png", .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "Ground Normal", .cpuNormalMipChain = true},
    }};
    VkCommandBuffer uploadCommands = VK_NULL_HANDLE;
    VkDeviceSize totalStagingSize = 0;
    Buffer textureStaging;

    const auto destroyLocalUploads = [&] {
        destroyBuffer(textureStaging);
        for (TextureUpload& upload : uploads) {
            destroyImage(upload.image);
        }
    };

    try {
        for (TextureUpload& upload : uploads) {
            LoadedImageRgba8 baseLevel = loadImageRgba8(upload.path);
            const VkExtent2D textureExtent{baseLevel.width, baseLevel.height};
            upload.baseWidth = baseLevel.width;
            upload.baseHeight = baseLevel.height;
            const std::uint32_t requestedMipLevels = mipLevelCountForExtent(textureExtent);
            const bool canLinearBlitMipmaps = requestedMipLevels > 1U && formatSupportsLinearMipBlit(upload.format);
            const bool alphaNeedsCpuMips = !upload.cpuNormalMipChain && requestedMipLevels > 1U && hasNonOpaqueAlpha(baseLevel);
            upload.gpuMipGeneration = !upload.cpuNormalMipChain && canLinearBlitMipmaps && !alphaNeedsCpuMips;
            if (upload.cpuNormalMipChain) {
                upload.mipLevels = buildNormalMapMipChainRgba8(std::move(baseLevel));
            } else if (alphaNeedsCpuMips) {
                upload.mipLevels = buildAlbedoMipChainRgba8(std::move(baseLevel), upload.format == VK_FORMAT_R8G8B8A8_SRGB);
                logger()->info("Texture {} uses alpha-weighted CPU albedo mips to avoid transparent texel bleed", upload.path.string());
            } else if (upload.gpuMipGeneration || requestedMipLevels == 1U) {
                upload.mipLevels = std::vector<LoadedImageRgba8>{std::move(baseLevel)};
            } else {
                upload.mipLevels = buildAlbedoMipChainRgba8(std::move(baseLevel), upload.format == VK_FORMAT_R8G8B8A8_SRGB);
                logger()->warn("Texture format {} lacks linear blit/filter support; generated {} CPU albedo mips for {}",
                               static_cast<int>(upload.format), upload.mipLevels.size(), upload.path.string());
            }
            const std::uint32_t imageMipLevels = upload.gpuMipGeneration ? requestedMipLevels : static_cast<std::uint32_t>(upload.mipLevels.size());

            upload.copyRegions.reserve(upload.mipLevels.size());
            for (std::uint32_t mipLevel = 0; mipLevel < upload.mipLevels.size(); ++mipLevel) {
                const LoadedImageRgba8& mip = upload.mipLevels[mipLevel];
                const VkDeviceSize mipBytes = static_cast<VkDeviceSize>(mip.pixels.size());
                if (mipBytes > std::numeric_limits<VkDeviceSize>::max() - totalStagingSize) {
                    throw std::runtime_error("Texture upload staging size exceeds VkDeviceSize range");
                }
                VkBufferImageCopy region{};
                region.bufferOffset = totalStagingSize;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = mipLevel;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {mip.width, mip.height, 1};
                upload.copyRegions.push_back(region);
                totalStagingSize += mipBytes;
            }

            VkImageUsageFlags textureUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (upload.gpuMipGeneration) {
                textureUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            upload.image = createImage(textureExtent, upload.format, textureUsage, VK_IMAGE_ASPECT_COLOR_BIT, imageMipLevels);
            const std::string textureName = upload.debugName + " Texture";
            const std::string allocationName = textureName + " Allocation";
            const std::string viewName = textureName + " View";
            setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(upload.image.image), textureName);
            vmaSetAllocationName(allocator_, upload.image.allocation, allocationName.c_str());
            setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(upload.image.view), viewName);
            upload.image.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Image, textureName,
                                                                         imageByteEstimate(upload.image.extent, upload.image.format, upload.image.mipLevels));
        }

        textureStaging = createBuffer(totalStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap stagingMap{allocator_, textureStaging.allocation, "vmaMapMemory texture staging"};
            auto* dst = static_cast<std::uint8_t*>(stagingMap.get());
            for (TextureUpload& upload : uploads) {
                for (const LoadedImageRgba8& mip : upload.mipLevels) {
                    std::memcpy(dst, mip.pixels.data(), mip.pixels.size());
                    dst += mip.pixels.size();
                }
                upload.mipLevels.clear();
            }
        }

        uploadCommands = beginGraphicsUploadCommands();
        for (TextureUpload& upload : uploads) {
            transitionImageTracked(uploadCommands, upload.image.image, upload.image.syncState,
                                   ImageSyncState{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                                   VK_IMAGE_ASPECT_COLOR_BIT, 0, upload.image.mipLevels);
            vkCmdCopyBufferToImage(uploadCommands, textureStaging.buffer, upload.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(upload.copyRegions.size()), upload.copyRegions.data());
            if (upload.gpuMipGeneration) {
                generateMipmaps(uploadCommands, upload.image);
            } else {
                transitionImageTracked(uploadCommands, upload.image.image, upload.image.syncState,
                                       imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::SampledImage),
                                       VK_IMAGE_ASPECT_COLOR_BIT, 0, upload.image.mipLevels);
            }
        }

        VkCommandBuffer submittedCommands = uploadCommands;
        uploadCommands = VK_NULL_HANDLE;
        submitGraphicsUpload(submittedCommands, takeBuffer(textureStaging));

        groundAlbedoTexture_ = takeImage(uploads[0].image);
        groundNormalTexture_ = takeImage(uploads[1].image);
        logger()->info("Loaded texture {} ({}x{} RGBA8, {} mips, format {})",
                       uploads[0].path.string(), uploads[0].baseWidth, uploads[0].baseHeight,
                       groundAlbedoTexture_.mipLevels, static_cast<int>(uploads[0].format));
        logger()->info("Loaded texture {} ({}x{} RGBA8, {} CPU-renormalized mips, format {})",
                       uploads[1].path.string(), uploads[1].baseWidth, uploads[1].baseHeight,
                       groundNormalTexture_.mipLevels, static_cast<int>(uploads[1].format));
    } catch (...) {
        if (uploadCommands != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device_, graphicsCommandPool_, 1, &uploadCommands);
        }
        destroyLocalUploads();
        throw;
    }
}

void VulkanRenderer::Impl::createSampler() {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &linearSampler_), "vkCreateSampler");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(linearSampler_), "Linear Clamp Sampler");

    VkSamplerCreateInfo textureSamplerInfo = samplerInfo;
    textureSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    const auto samplerMaxLod = [](const std::uint32_t mipLevels) {
        return mipLevels > 0U ? static_cast<float>(mipLevels - 1U) : 0.0f;
    };
    textureSamplerInfo.maxLod = samplerMaxLod(groundAlbedoTexture_.mipLevels);
    textureSamplerInfo.anisotropyEnable = samplerAnisotropyEnabled_ ? VK_TRUE : VK_FALSE;
    textureSamplerInfo.maxAnisotropy = maxSamplerAnisotropy_;
    checkVk(vkCreateSampler(device_, &textureSamplerInfo, nullptr, &textureSampler_), "vkCreateSampler texture");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(textureSampler_), "Linear Repeat Albedo Texture Sampler");

    VkSamplerCreateInfo normalSamplerInfo = textureSamplerInfo;
    normalSamplerInfo.maxLod = samplerMaxLod(groundNormalTexture_.mipLevels);
    normalSamplerInfo.anisotropyEnable = VK_FALSE;
    normalSamplerInfo.maxAnisotropy = 1.0f;
    checkVk(vkCreateSampler(device_, &normalSamplerInfo, nullptr, &normalTextureSampler_), "vkCreateSampler normal texture");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(normalTextureSampler_), "Linear Repeat Normal Texture Sampler");
}

void VulkanRenderer::Impl::createDescriptorLayouts() {
    std::array<VkDescriptorSetLayoutBinding, 3> sceneBindings{};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[1].binding = 1;
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneBindings[1].descriptorCount = 2;
    sceneBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[2].binding = 2;
    sceneBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[2].descriptorCount = 1;
    sceneBindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo sceneInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sceneInfo.bindingCount = static_cast<std::uint32_t>(sceneBindings.size());
    sceneInfo.pBindings = sceneBindings.data();
    checkVk(vkCreateDescriptorSetLayout(device_, &sceneInfo, nullptr, &sceneSetLayout_), "vkCreateDescriptorSetLayout scene");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, handleToUint64(sceneSetLayout_), "Scene Descriptor Set Layout");

    VkDescriptorSetLayoutBinding tonemapBinding{};
    tonemapBinding.binding = 0;
    tonemapBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tonemapBinding.descriptorCount = 1;
    tonemapBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo tonemapInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tonemapInfo.bindingCount = 1;
    tonemapInfo.pBindings = &tonemapBinding;
    checkVk(vkCreateDescriptorSetLayout(device_, &tonemapInfo, nullptr, &tonemapSetLayout_), "vkCreateDescriptorSetLayout tonemap");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, handleToUint64(tonemapSetLayout_), "Tonemap Descriptor Set Layout");

    // Renderer-owned descriptor sets are allocated once at startup; the ImGui backend owns a
    // separate pool so scene/tonemap descriptor pressure stays fixed and free of frame-loop churn.
    constexpr std::uint32_t sceneSetCount = kMaxFramesInFlight;
    constexpr std::uint32_t tonemapSetCount = 1U;
    constexpr std::uint32_t rendererSetCount = sceneSetCount + tonemapSetCount;
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sceneSetCount};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (sceneSetCount * 2U) + tonemapSetCount};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sceneSetCount};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = rendererSetCount;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, handleToUint64(descriptorPool_), "Renderer Descriptor Pool");
}

VulkanRenderer::Impl::Buffer VulkanRenderer::Impl::createBuffer(const VkDeviceSize size, const VkBufferUsageFlags usage, const VkMemoryPropertyFlags properties, const bool sharedGraphicsTransfer, const VmaAllocationCreateFlags hostAccessFlags) {
    Buffer buffer{};
    buffer.size = size;
    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = size;
    createInfo.usage = usage;
    const std::array<std::uint32_t, 2> queueFamilies{queueFamilies_.graphics.value(), queueFamilies_.transfer.value()};
    if (sharedGraphicsTransfer && queueFamilies[0] != queueFamilies[1]) {
        createInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilies.size());
        createInfo.pQueueFamilyIndices = queueFamilies.data();
    } else {
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.requiredFlags = properties;
    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
        allocationInfo.flags |= hostAccessFlags;
    }

    checkVk(vmaCreateBuffer(allocator_, &createInfo, &allocationInfo, &buffer.buffer, &buffer.allocation, nullptr), "vmaCreateBuffer");
    return buffer;
}

void VulkanRenderer::Impl::destroyBuffer(Buffer& buffer) {
    resourceRegistry_.unregisterResource(buffer.resourceId);
    buffer.resourceId = GpuResourceRegistry::kInvalidId;
    if (buffer.mapped != nullptr) {
        vmaUnmapMemory(allocator_, buffer.allocation);
    }
    if (buffer.buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation); }
    buffer = {};
}

void VulkanRenderer::Impl::createTonemapDescriptorSet() {
    if (tonemapDescriptorSet_ == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = descriptorPool_;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &tonemapSetLayout_;
        checkVk(vkAllocateDescriptorSets(device_, &allocInfo, &tonemapDescriptorSet_), "vkAllocateDescriptorSets tonemap");
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, handleToUint64(tonemapDescriptorSet_), "Tonemap Descriptor Set");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = linearSampler_;
    imageInfo.imageView = hdr_.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = tonemapDescriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

} // namespace ve
