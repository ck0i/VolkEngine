#include "renderer/vulkan/VulkanRendererImpl.hpp"
#include "assets/DerivedDataCache.hpp"
#include "assets/TextureArtifact.hpp"

#include <cstring>

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
    checkVk(vkCreateImageView(deviceOwner_.device, &viewInfo, nullptr, &view), "vkCreateImageView");
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
    checkVk(vmaCreateImage(deviceOwner_.allocator, &imageInfo, &allocationInfo, &resource.image, &resource.allocation, nullptr), "vmaCreateImage");
    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(deviceOwner_.device, resource.image, &memoryRequirements);
    resource.allocationBytes = memoryRequirements.size;
    resource.allocationAlignment = memoryRequirements.alignment;

    try {
        resource.view = createImageView(resource.image, format, aspectFlags, mipLevels);
    } catch (...) {
        destroyImage(resource);
        throw;
    }
    return resource;
}

void VulkanRenderer::Impl::destroyImage(ImageResource& image) {
    resourceOwner_.registry.unregisterResource(image.resourceId);
    image.resourceId = GpuResourceRegistry::kInvalidId;
    if (image.view != VK_NULL_HANDLE) { vkDestroyImageView(deviceOwner_.device, image.view, nullptr); }
    if (image.image != VK_NULL_HANDLE) { vmaDestroyImage(deviceOwner_.allocator, image.image, image.allocation); }
    image = {};
}

void VulkanRenderer::Impl::createTextureResources() {
    enum class TextureMipPolicy : std::uint8_t {
        Albedo,
        Normal,
        Linear
    };

    struct TextureUpload {
        std::filesystem::path path;
        TextureArtifact artifact;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::string debugName;
        TextureMipPolicy mipPolicy = TextureMipPolicy::Albedo;
        TextureRole role = TextureRole::BaseColor;
        std::uint32_t baseWidth = 0;
        std::uint32_t baseHeight = 0;
        std::vector<LoadedImageRgba8> mipLevels;
        std::vector<VkBufferImageCopy> copyRegions;
        bool gpuMipGeneration = false;
        ImageResource image;
    };

    const ReferenceAssetBundle& authored = referenceAssets();
    if (authored.scene.materials.empty()) {
        throw std::runtime_error("Authored reference scene contains no material");
    }
    std::vector<TextureUpload> uploads;
    std::size_t textureCount = 0;
    for (const ImportedMaterial& material : authored.scene.materials) {
        textureCount += material.textures.size();
    }
    uploads.reserve(textureCount);
    resourceOwner_.referenceMaterialTextureIndices.fill(std::numeric_limits<std::size_t>::max());
    resourceOwner_.materialTextureBindings.clear();
    resourceOwner_.materialTextureBindings.reserve(authored.scene.materials.size());
    DerivedDataCache textureCache{config_.cacheDirectory / "assets" / "ddc"};
    for (std::size_t materialIndex = 0; materialIndex < authored.scene.materials.size();
         ++materialIndex) {
        const ImportedMaterial& material = authored.scene.materials[materialIndex];
        MaterialTextureBinding binding;
        binding.material = material.id;
        for (const ImportedTextureReference& reference : material.textures) {
            auto existing = std::ranges::find_if(uploads, [&](const TextureUpload& upload) {
                return upload.path == reference.sourcePath && upload.role == reference.role &&
                       upload.format == (reference.colorSpace == TextureColorSpace::Srgb
                                             ? VK_FORMAT_R8G8B8A8_SRGB
                                             : VK_FORMAT_R8G8B8A8_UNORM);
            });
            std::size_t textureIndex = static_cast<std::size_t>(existing - uploads.begin());
            if (existing == uploads.end()) {
                const AssetRecord* record = authored.database.find(reference.id);
                if (record == nullptr || record->type != AssetType::Texture) {
                    throw std::runtime_error("Authored texture has no asset database record: " +
                                             reference.sourcePath.generic_string());
                }
                TextureUpload upload;
                upload.path = reference.sourcePath;
                upload.role = reference.role;
                upload.format = reference.colorSpace == TextureColorSpace::Srgb
                                    ? VK_FORMAT_R8G8B8A8_SRGB
                                    : VK_FORMAT_R8G8B8A8_UNORM;
                upload.debugName = "Authored " + reference.sourcePath.filename().string();
                upload.mipPolicy = reference.role == TextureRole::Normal
                                       ? TextureMipPolicy::Normal
                                       : (reference.colorSpace == TextureColorSpace::Srgb
                                              ? TextureMipPolicy::Albedo
                                              : TextureMipPolicy::Linear);
                upload.artifact = deserializeTextureArtifact(
                    textureCache.load(record->artifactKey, ArtifactType::Texture,
                                      record->artifactSchemaVersion).payload);
                if (upload.artifact.id != reference.id ||
                    upload.artifact.role != reference.role ||
                    upload.artifact.colorSpace != reference.colorSpace) {
                    throw std::runtime_error(
                        "Authored texture artifact metadata does not match its material reference");
                }
                uploads.push_back(std::move(upload));
                textureIndex = uploads.size() - 1U;
            }
            if (textureIndex > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("Material texture descriptor index range exhausted");
            }
            const TextureAssetHandle handle{
                static_cast<std::uint32_t>(textureIndex), 1U};
            switch (reference.role) {
            case TextureRole::BaseColor: binding.textures[0] = handle; break;
            case TextureRole::Normal: binding.textures[1] = handle; break;
            case TextureRole::MetallicRoughness: binding.textures[2] = handle; break;
            case TextureRole::Occlusion:
            case TextureRole::Emissive: break;
            }
            if (materialIndex == 0U) {
                switch (reference.role) {
                case TextureRole::BaseColor: resourceOwner_.referenceMaterialTextureIndices[0] = textureIndex; break;
                case TextureRole::Normal: resourceOwner_.referenceMaterialTextureIndices[1] = textureIndex; break;
                case TextureRole::MetallicRoughness: resourceOwner_.referenceMaterialTextureIndices[2] = textureIndex; break;
                case TextureRole::Occlusion:
                case TextureRole::Emissive: break;
                }
            }
        }
        resourceOwner_.materialTextureBindings.push_back(binding);
    }
    if (uploads.empty()) {
        throw std::runtime_error("Authored reference scene contains no texture assets");
    }
    for (std::size_t& textureIndex : resourceOwner_.referenceMaterialTextureIndices) {
        if (textureIndex == std::numeric_limits<std::size_t>::max()) textureIndex = 0U;
    }
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
            if (upload.artifact.storage != TextureStorage::Rgba8 ||
                upload.artifact.mips.size() != 1U) {
                throw std::runtime_error(
                    "Authored texture storage is cooked but unsupported by the current Vulkan material upload path: " +
                    upload.path.generic_string());
            }
            LoadedImageRgba8 baseLevel;
            baseLevel.width = upload.artifact.width;
            baseLevel.height = upload.artifact.height;
            baseLevel.pixels.resize(upload.artifact.data.size());
            std::memcpy(baseLevel.pixels.data(), upload.artifact.data.data(),
                        upload.artifact.data.size());
            const VkExtent2D textureExtent{baseLevel.width, baseLevel.height};
            if (!vulkan_renderer_detail::textureExtentFitsDeviceLimit(textureExtent, deviceOwner_.info.maxImageDimension2D)) {
                throw std::runtime_error("Texture " + upload.path.string() + " is " + std::to_string(textureExtent.width) + "x" +
                                         std::to_string(textureExtent.height) + ", exceeding device maxImageDimension2D " +
                                         std::to_string(deviceOwner_.info.maxImageDimension2D));
            }
            upload.baseWidth = baseLevel.width;
            upload.baseHeight = baseLevel.height;
            const std::uint32_t requestedMipLevels = mipLevelCountForExtent(textureExtent);
            const bool canLinearBlitMipmaps = requestedMipLevels > 1U && formatSupportsLinearMipBlit(upload.format);
            const bool isAlbedo = upload.mipPolicy == TextureMipPolicy::Albedo;
            const bool isNormal = upload.mipPolicy == TextureMipPolicy::Normal;
            const bool alphaNeedsCpuMips = isAlbedo && requestedMipLevels > 1U && hasNonOpaqueAlpha(baseLevel);
            upload.gpuMipGeneration = !isNormal && canLinearBlitMipmaps && !alphaNeedsCpuMips;
            if (isNormal) {
                upload.mipLevels = buildNormalMapMipChainRgba8(std::move(baseLevel));
            } else if (alphaNeedsCpuMips) {
                upload.mipLevels = buildAlbedoMipChainRgba8(std::move(baseLevel), upload.format == VK_FORMAT_R8G8B8A8_SRGB);
                logger()->info("Texture {} uses alpha-weighted CPU albedo mips to avoid transparent texel bleed", upload.path.string());
            } else if (upload.gpuMipGeneration || requestedMipLevels == 1U) {
                upload.mipLevels = std::vector<LoadedImageRgba8>{std::move(baseLevel)};
            } else if (isAlbedo) {
                upload.mipLevels = buildAlbedoMipChainRgba8(std::move(baseLevel), upload.format == VK_FORMAT_R8G8B8A8_SRGB);
                logger()->warn("Texture format {} lacks linear blit/filter support; generated {} CPU albedo mips for {}",
                               static_cast<int>(upload.format), upload.mipLevels.size(), upload.path.string());
            } else {
                upload.mipLevels = buildLinearMipChainRgba8(std::move(baseLevel));
                logger()->warn("Texture format {} lacks linear blit/filter support; generated {} straight RGBA mips for {}",
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
            vmaSetAllocationName(deviceOwner_.allocator, upload.image.allocation, allocationName.c_str());
            setObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, handleToUint64(upload.image.view), viewName);
            upload.image.resourceId = resourceOwner_.registry.registerResource(GpuResourceKind::Image, textureName,
                                                                         imageByteEstimate(upload.image.extent, upload.image.format, upload.image.mipLevels));
        }

        textureStaging = createBuffer(totalStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        {
            ScopedVmaMap stagingMap{deviceOwner_.allocator, textureStaging.allocation, "vmaMapMemory texture staging"};
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

        resourceOwner_.materialTextures.clear();
        resourceOwner_.materialTextures.reserve(uploads.size());
        resourceOwner_.materialTextureRoles.clear();
        resourceOwner_.materialTextureRoles.reserve(uploads.size());
        for (TextureUpload& upload : uploads) {
            resourceOwner_.materialTextureRoles.push_back(upload.role);
            resourceOwner_.materialTextures.push_back(takeImage(upload.image));
            logger()->info("Loaded authored texture {} ({}x{}, {} mips, format {})",
                           upload.path.string(), upload.baseWidth, upload.baseHeight,
                           resourceOwner_.materialTextures.back().mipLevels,
                           static_cast<int>(upload.format));
        }
    } catch (...) {
        if (uploadCommands != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(deviceOwner_.device, frameOwner_.graphicsCommandPool, 1, &uploadCommands);
        }
        destroyLocalUploads();
        throw;
    }
}

std::array<TextureAssetHandle, 3> VulkanRenderer::Impl::materialTextureHandles(
    const AssetId material) const {
    const auto found = std::ranges::find(
        resourceOwner_.materialTextureBindings, material,
        &MaterialTextureBinding::material);
    if (found == resourceOwner_.materialTextureBindings.end()) {
        throw std::runtime_error("Authored material has no GPU texture binding");
    }
    return found->textures;
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
    checkVk(vkCreateSampler(deviceOwner_.device, &samplerInfo, nullptr, &resourceOwner_.linearSampler), "vkCreateSampler");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(resourceOwner_.linearSampler), "Linear Clamp Sampler");
    VkFormatProperties depthProperties{};
    VkFormatProperties pyramidProperties{};
    vkGetPhysicalDeviceFormatProperties(
        deviceOwner_.physicalDevice, resourceOwner_.depth.format,
        &depthProperties);
    vkGetPhysicalDeviceFormatProperties(
        deviceOwner_.physicalDevice, resourceOwner_.depthPyramid.format,
        &pyramidProperties);
    constexpr VkFormatFeatureFlags kMinmaxFeature =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT;
    resourceOwner_.depthReductionSamplerEnabled =
        deviceOwner_.info.samplerFilterMinmax &&
        (depthProperties.optimalTilingFeatures & kMinmaxFeature) != 0U &&
        (pyramidProperties.optimalTilingFeatures & kMinmaxFeature) != 0U;
    if (resourceOwner_.depthReductionSamplerEnabled) {
        VkSamplerReductionModeCreateInfo reductionInfo{
            VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
        reductionInfo.reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN;
        VkSamplerCreateInfo reductionSamplerInfo = samplerInfo;
        reductionSamplerInfo.pNext = &reductionInfo;
        checkVk(vkCreateSampler(deviceOwner_.device, &reductionSamplerInfo,
                                nullptr,
                                &resourceOwner_.depthReductionSampler),
                "vkCreateSampler depth reduction");
        setObjectName(VK_OBJECT_TYPE_SAMPLER,
                      handleToUint64(resourceOwner_.depthReductionSampler),
                      "Depth Pyramid Min Reduction Sampler");
    }

    VkSamplerCreateInfo textureSamplerInfo = samplerInfo;
    textureSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    textureSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    textureSamplerInfo.anisotropyEnable = resourceOwner_.samplerAnisotropyEnabled ? VK_TRUE : VK_FALSE;
    textureSamplerInfo.maxAnisotropy = resourceOwner_.maxSamplerAnisotropy;
    checkVk(vkCreateSampler(deviceOwner_.device, &textureSamplerInfo, nullptr, &resourceOwner_.textureSampler), "vkCreateSampler texture");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(resourceOwner_.textureSampler), "Linear Repeat Albedo Texture Sampler");

    VkSamplerCreateInfo normalSamplerInfo = textureSamplerInfo;
    normalSamplerInfo.anisotropyEnable = VK_FALSE;
    normalSamplerInfo.maxAnisotropy = 1.0f;
    checkVk(vkCreateSampler(deviceOwner_.device, &normalSamplerInfo, nullptr, &resourceOwner_.normalTextureSampler), "vkCreateSampler normal texture");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(resourceOwner_.normalTextureSampler), "Linear Repeat Normal Texture Sampler");
    VkSamplerCreateInfo ormSamplerInfo = textureSamplerInfo;
    checkVk(vkCreateSampler(deviceOwner_.device, &ormSamplerInfo, nullptr, &resourceOwner_.ormTextureSampler), "vkCreateSampler ORM texture");
    setObjectName(VK_OBJECT_TYPE_SAMPLER, handleToUint64(resourceOwner_.ormTextureSampler), "Linear Repeat ORM Texture Sampler");
}

void VulkanRenderer::Impl::createDescriptorLayouts() {
    const auto& limits = deviceOwner_.physicalDeviceProperties.limits;
    const std::uint32_t descriptorLimit = std::min({
        limits.maxDescriptorSetSampledImages,
        limits.maxPerStageDescriptorSampledImages,
        limits.maxDescriptorSetSamplers,
        limits.maxPerStageDescriptorSamplers,
        4096U});
    if (resourceOwner_.materialTextures.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Material texture descriptor index range exhausted");
    }
    const std::uint32_t textureCount =
        static_cast<std::uint32_t>(resourceOwner_.materialTextures.size());
    resourceOwner_.bindlessMaterialsEnabled =
        deviceOwner_.info.bindlessSampledImagesSupported &&
        textureCount <= descriptorLimit;
    if (!resourceOwner_.bindlessMaterialsEnabled &&
        textureCount > vulkan_renderer_detail::kMaterialTextureCount) {
        throw std::runtime_error(
            "Scene requires bindless sampled images, but the device feature or descriptor limit is unavailable");
    }
    resourceOwner_.materialDescriptorCapacity =
        resourceOwner_.bindlessMaterialsEnabled
            ? descriptorLimit
            : vulkan_renderer_detail::kMaterialTextureCount;
    std::array<VkDescriptorSetLayoutBinding, 4> sceneBindings{};
    sceneBindings[0].binding = 0;
    sceneBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sceneBindings[0].descriptorCount = 1;
    sceneBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[1].binding = 3;
    if (!resourceOwner_.bindlessMaterialsEnabled) {
        for (const MaterialTextureBinding& binding :
             resourceOwner_.materialTextureBindings) {
            for (std::size_t role = 0; role < binding.textures.size(); ++role) {
                if (binding.textures[role].valid() &&
                    binding.textures[role].index !=
                        resourceOwner_.referenceMaterialTextureIndices[role]) {
                    throw std::runtime_error(
                        "Fixed material descriptor fallback cannot represent this scene's texture bindings");
                }
            }
        }
    }
    sceneBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sceneBindings[1].descriptorCount = resourceOwner_.materialDescriptorCapacity;
    sceneBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    sceneBindings[2].binding = 1;
    sceneBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[2].descriptorCount = 1;
    sceneBindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    sceneBindings[3].binding = 2;
    sceneBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sceneBindings[3].descriptorCount = 1;
    sceneBindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    std::array<VkDescriptorBindingFlags, 4> sceneBindingFlags{};
    if (resourceOwner_.bindlessMaterialsEnabled) {
        sceneBindingFlags[1] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                               VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo sceneBindingFlagsInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    sceneBindingFlagsInfo.bindingCount =
        static_cast<std::uint32_t>(sceneBindingFlags.size());
    sceneBindingFlagsInfo.pBindingFlags = sceneBindingFlags.data();

    VkDescriptorSetLayoutCreateInfo sceneInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sceneInfo.bindingCount = static_cast<std::uint32_t>(sceneBindings.size());
    sceneInfo.pBindings = sceneBindings.data();
    if (resourceOwner_.bindlessMaterialsEnabled) {
        sceneInfo.pNext = &sceneBindingFlagsInfo;
    }
    checkVk(vkCreateDescriptorSetLayout(deviceOwner_.device, &sceneInfo, nullptr, &resourceOwner_.sceneSetLayout), "vkCreateDescriptorSetLayout scene");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, handleToUint64(resourceOwner_.sceneSetLayout), "Scene Descriptor Set Layout");

    VkDescriptorSetLayoutBinding tonemapBinding{};
    tonemapBinding.binding = 0;
    tonemapBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tonemapBinding.descriptorCount = 1;
    tonemapBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo tonemapInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tonemapInfo.bindingCount = 1;
    tonemapInfo.pBindings = &tonemapBinding;
    checkVk(vkCreateDescriptorSetLayout(deviceOwner_.device, &tonemapInfo, nullptr, &resourceOwner_.tonemapSetLayout), "vkCreateDescriptorSetLayout tonemap");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, handleToUint64(resourceOwner_.tonemapSetLayout), "Tonemap Descriptor Set Layout");
    std::array<VkDescriptorSetLayoutBinding, 8> cullBindings{};
    for (std::uint32_t binding = 0; binding < 5U; ++binding) {
        cullBindings[binding].binding = binding;
        cullBindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cullBindings[binding].descriptorCount = 1;
        cullBindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    cullBindings[5].binding = 5;
    cullBindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cullBindings[5].descriptorCount = 1;
    cullBindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cullBindings[6].binding = 6;
    cullBindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cullBindings[6].descriptorCount = 1;
    cullBindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    cullBindings[7].binding = 7;
    cullBindings[7].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cullBindings[7].descriptorCount = 1;
    cullBindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo cullInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cullInfo.bindingCount = static_cast<std::uint32_t>(cullBindings.size());
    cullInfo.pBindings = cullBindings.data();
    checkVk(vkCreateDescriptorSetLayout(deviceOwner_.device, &cullInfo, nullptr,
                                        &resourceOwner_.cullSetLayout),
            "vkCreateDescriptorSetLayout scene cull");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                  handleToUint64(resourceOwner_.cullSetLayout),
                  "Scene Cull Descriptor Set Layout");
    std::array<VkDescriptorSetLayoutBinding, 2> depthPyramidBindings{};
    depthPyramidBindings[0].binding = 0;
    depthPyramidBindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    depthPyramidBindings[0].descriptorCount = 1;
    depthPyramidBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    depthPyramidBindings[1].binding = 1;
    depthPyramidBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    depthPyramidBindings[1].descriptorCount = 1;
    depthPyramidBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo depthPyramidInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    depthPyramidInfo.bindingCount =
        static_cast<std::uint32_t>(depthPyramidBindings.size());
    depthPyramidInfo.pBindings = depthPyramidBindings.data();
    checkVk(vkCreateDescriptorSetLayout(
                deviceOwner_.device, &depthPyramidInfo, nullptr,
                &resourceOwner_.depthPyramidSetLayout),
            "vkCreateDescriptorSetLayout depth pyramid");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                  handleToUint64(resourceOwner_.depthPyramidSetLayout),
                  "Depth Pyramid Descriptor Set Layout");

    // Renderer-owned descriptor sets are allocated once at startup; the ImGui backend owns a
    // separate pool so scene/tonemap descriptor pressure stays fixed and free of frame-loop churn.
    constexpr std::uint32_t sceneSetCount = kMaxFramesInFlight;
    constexpr std::uint32_t tonemapSetCount = 1U;
    constexpr std::uint32_t cullSetCount = kMaxFramesInFlight;
    constexpr std::uint32_t depthPyramidSetCount =
        static_cast<std::uint32_t>(kMaxDepthPyramidMipLevels);
    constexpr std::uint32_t rendererSetCount =
        sceneSetCount + tonemapSetCount + cullSetCount +
        depthPyramidSetCount;
    std::array<VkDescriptorPoolSize, 4> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    sceneSetCount + cullSetCount};
    const std::uint32_t descriptorsPerSceneSet =
        resourceOwner_.bindlessMaterialsEnabled
            ? textureCount
            : vulkan_renderer_detail::kMaterialTextureCount;
    poolSizes[1] = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        sceneSetCount * descriptorsPerSceneSet + tonemapSetCount +
            cullSetCount + depthPyramidSetCount};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    sceneSetCount * 2U + cullSetCount * 6U};
    poolSizes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    depthPyramidSetCount};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = rendererSetCount;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    checkVk(vkCreateDescriptorPool(deviceOwner_.device, &poolInfo, nullptr, &resourceOwner_.descriptorPool), "vkCreateDescriptorPool");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, handleToUint64(resourceOwner_.descriptorPool), "Renderer Descriptor Pool");
}

VulkanRenderer::Impl::Buffer VulkanRenderer::Impl::createBuffer(const VkDeviceSize size, const VkBufferUsageFlags usage, const VkMemoryPropertyFlags properties, const bool sharedGraphicsTransfer, const VmaAllocationCreateFlags hostAccessFlags) {
    Buffer buffer{};
    buffer.size = size;
    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = size;
    createInfo.usage = usage;
    const std::array<std::uint32_t, 2> queueFamilies{deviceOwner_.queueFamilies.graphics.value(), deviceOwner_.queueFamilies.transfer.value()};
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

    checkVk(vmaCreateBuffer(deviceOwner_.allocator, &createInfo, &allocationInfo, &buffer.buffer, &buffer.allocation, nullptr), "vmaCreateBuffer");
    return buffer;
}

void VulkanRenderer::Impl::destroyBuffer(Buffer& buffer) {
    resourceOwner_.registry.unregisterResource(buffer.resourceId);
    buffer.resourceId = GpuResourceRegistry::kInvalidId;
    if (buffer.mapped != nullptr) {
        vmaUnmapMemory(deviceOwner_.allocator, buffer.allocation);
    }
    if (buffer.buffer != VK_NULL_HANDLE) { vmaDestroyBuffer(deviceOwner_.allocator, buffer.buffer, buffer.allocation); }
    buffer = {};
}

void VulkanRenderer::Impl::createTonemapDescriptorSet() {
    if (resourceOwner_.tonemapDescriptorSet == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocInfo.descriptorPool = resourceOwner_.descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &resourceOwner_.tonemapSetLayout;
        checkVk(vkAllocateDescriptorSets(deviceOwner_.device, &allocInfo, &resourceOwner_.tonemapDescriptorSet), "vkAllocateDescriptorSets tonemap");
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, handleToUint64(resourceOwner_.tonemapDescriptorSet), "Tonemap Descriptor Set");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = resourceOwner_.linearSampler;
    imageInfo.imageView = resourceOwner_.hdr.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = resourceOwner_.tonemapDescriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(deviceOwner_.device, 1, &write, 0, nullptr);
}

} // namespace ve
