#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

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
    const std::filesystem::path texturePath = config_.assetDirectory / "textures" / "ground_albedo.ppm";
    const LoadedImageRgba8 texture = loadPpmRgba8(texturePath);
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(texture.pixels.size());

    Buffer staging{};
    std::vector<Buffer> stagingBuffers;
    stagingBuffers.reserve(1);
    try {
        staging = createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap stagingMap{allocator_, staging.allocation, "vmaMapMemory texture staging"};
            std::memcpy(stagingMap.get(), texture.pixels.data(), texture.pixels.size());
        }

        const VkExtent2D textureExtent{texture.width, texture.height};
        const VkFormat textureFormat = VK_FORMAT_R8G8B8A8_SRGB;
        const std::uint32_t requestedMipLevels = mipLevelCountForExtent(textureExtent);
        const bool canGenerateMipmaps = requestedMipLevels > 1U && formatSupportsLinearMipBlit(textureFormat);
        const std::uint32_t mipLevels = canGenerateMipmaps ? requestedMipLevels : 1U;
        if (requestedMipLevels > 1U && !canGenerateMipmaps) {
            logger()->warn("Texture format VK_FORMAT_R8G8B8A8_SRGB lacks linear blit/filter support; loading {} with one mip", texturePath.string());
        }
        VkImageUsageFlags textureUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (mipLevels > 1U) {
            textureUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        groundAlbedoTexture_ = createImage(textureExtent, textureFormat, textureUsage, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
        setObjectName(VK_OBJECT_TYPE_IMAGE, handleToUint64(groundAlbedoTexture_.image), "Ground Albedo Texture");
        vmaSetAllocationName(allocator_, groundAlbedoTexture_.allocation, "Ground Albedo Texture Allocation");
        setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(groundAlbedoTexture_.view), "Ground Albedo Texture View");
        groundAlbedoTexture_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Image, "Ground Albedo Texture",
                                                                             imageByteEstimate(groundAlbedoTexture_.extent, groundAlbedoTexture_.format, groundAlbedoTexture_.mipLevels));

        VkCommandBuffer uploadCommands = beginGraphicsUploadCommands();
        transitionImageTracked(uploadCommands, groundAlbedoTexture_.image, groundAlbedoTexture_.syncState,
                               ImageSyncState{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                               VK_IMAGE_ASPECT_COLOR_BIT, 0, groundAlbedoTexture_.mipLevels);
        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {texture.width, texture.height, 1};
        vkCmdCopyBufferToImage(uploadCommands, staging.buffer, groundAlbedoTexture_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        if (groundAlbedoTexture_.mipLevels > 1U) {
            generateMipmaps(uploadCommands, groundAlbedoTexture_);
        } else {
            transitionImageTracked(uploadCommands, groundAlbedoTexture_.image, groundAlbedoTexture_.syncState,
                                   imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::SampledImage),
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        }
        stagingBuffers.push_back(staging);
        staging = {};
        submitGraphicsUpload(uploadCommands, std::move(stagingBuffers));
        logger()->info("Loaded texture {} ({}x{} RGBA8, {} mips)", texturePath.string(), texture.width, texture.height, groundAlbedoTexture_.mipLevels);
    } catch (...) {
        destroyBuffer(staging);
        destroyImage(groundAlbedoTexture_);
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
    textureSamplerInfo.maxLod = groundAlbedoTexture_.mipLevels > 0U ? static_cast<float>(groundAlbedoTexture_.mipLevels - 1U) : 0.0f;
    textureSamplerInfo.anisotropyEnable = samplerAnisotropyEnabled_ ? VK_TRUE : VK_FALSE;
    textureSamplerInfo.maxAnisotropy = maxSamplerAnisotropy_;
    checkVk(vkCreateSampler(device_, &textureSamplerInfo, nullptr, &textureSampler_), "vkCreateSampler texture");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(textureSampler_), "Linear Repeat Texture Sampler");
}

void VulkanRenderer::Impl::createDescriptorLayouts() {
    std::array<VkDescriptorSetLayoutBinding, 3> sceneBindings{};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[1].binding = 1;
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneBindings[1].descriptorCount = 1;
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
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sceneSetCount + tonemapSetCount};
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
