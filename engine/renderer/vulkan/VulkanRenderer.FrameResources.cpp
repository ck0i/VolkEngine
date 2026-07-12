#include "renderer/vulkan/VulkanRendererImpl.hpp"
#include <chrono>

namespace ve {

VkDeviceSize VulkanRenderer::Impl::checkedSceneInstanceBufferSize(std::size_t capacity) const {
    if (capacity == 0U) {
        capacity = 1U;
    }
    if (capacity > static_cast<std::size_t>(std::numeric_limits<VkDeviceSize>::max() / sizeof(InstanceData))) {
        throw std::runtime_error("Scene instance capacity exceeds Vulkan buffer size range");
    }

    const VkDeviceSize instanceBufferSize = static_cast<VkDeviceSize>(sizeof(InstanceData) * capacity);
    if (instanceBufferSize > deviceOwner_.physicalDeviceProperties.limits.maxStorageBufferRange) {
        throw std::runtime_error("Scene instance storage exceeds "
                             "VkPhysicalDeviceLimits::maxStorageBufferRange");
    }
    return instanceBufferSize;
}

void VulkanRenderer::Impl::createFrameInstanceDataBuffer(FrameResources& frame, const std::size_t frameIndex, std::size_t capacity) {
    if (capacity == 0U) {
        capacity = 1U;
    }
    const VkDeviceSize instanceBufferSize = checkedSceneInstanceBufferSize(capacity);

    Buffer instanceData;
    try {
        instanceData = createBuffer(instanceBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const std::string frameName = "Frame " + std::to_string(frameIndex);
        setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(instanceData.buffer), frameName + " Instance Data Buffer");
        const std::string instanceAllocationName = frameName + " Instance Data Allocation";
        vmaSetAllocationName(deviceOwner_.allocator, instanceData.allocation, instanceAllocationName.c_str());
        instanceData.resourceId = resourceOwner_.registry.registerResource(GpuResourceKind::Buffer, instanceAllocationName, instanceData.size);
        checkVk(vmaMapMemory(deviceOwner_.allocator, instanceData.allocation, &instanceData.mapped), "vmaMapMemory instance data");
    } catch (...) {
        destroyBuffer(instanceData);
        throw;
    }

    frame.instanceData = takeBuffer(instanceData);
    frame.instanceCapacity = capacity;
}

void VulkanRenderer::Impl::updateFrameInstanceDataDescriptor(const std::size_t frameIndex) const {
    const FrameResources& frame = frameOwner_.frames[frameIndex];
    VkDescriptorBufferInfo instanceBufferInfo{};
    instanceBufferInfo.buffer = frame.instanceData.buffer;
    instanceBufferInfo.offset = 0;
    instanceBufferInfo.range = frame.instanceData.size;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = resourceOwner_.sceneDescriptorSets[frameIndex];
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &instanceBufferInfo;
    vkUpdateDescriptorSets(deviceOwner_.device, 1, &write, 0, nullptr);
}
void VulkanRenderer::Impl::updateFrameVisibleInstanceDescriptor(
    const std::size_t frameIndex) const {
    const FrameResources& frame = frameOwner_.frames[frameIndex];
    VkDescriptorBufferInfo info{
        frame.visibleInstanceIndices.buffer, 0,
        frame.visibleInstanceIndices.size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = resourceOwner_.sceneDescriptorSets[frameIndex];
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(deviceOwner_.device, 1, &write, 0, nullptr);
}

void VulkanRenderer::Impl::updateFrameCullDescriptors(
    const std::size_t frameIndex) const {
    const FrameResources& frame = frameOwner_.frames[frameIndex];
    const std::array<VkDescriptorBufferInfo, 7> infos{{
        {frame.cullCandidates.buffer, 0, frame.cullCandidates.size},
        {frame.visibleInstanceIndices.buffer, 0,
         frame.visibleInstanceIndices.size},
        {frame.indirectCommands.buffer, 0, frame.indirectCommands.size},
        {resourceOwner_.clusterData.buffer, 0, resourceOwner_.clusterData.size},
        {resourceOwner_.meshClusterRanges.buffer, 0,
         resourceOwner_.meshClusterRanges.size},
        {frame.cullUniforms.buffer, 0, sizeof(GpuCullUniforms)},
        {frame.cullCounters.buffer, 0, sizeof(GpuCullCounters)},
    }};
    std::array<VkWriteDescriptorSet, 7> writes{};
    for (std::uint32_t binding = 0;
         binding < static_cast<std::uint32_t>(writes.size()); ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[binding].dstSet = resourceOwner_.cullDescriptorSets[frameIndex];
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType =
            binding == 5U ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(deviceOwner_.device,
                           static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    VkDescriptorImageInfo depthPyramidInfo{
        resourceOwner_.linearSampler, resourceOwner_.depthPyramid.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet depthPyramidWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    depthPyramidWrite.dstSet =
        resourceOwner_.cullDescriptorSets[frameIndex];
    depthPyramidWrite.dstBinding = 7;
    depthPyramidWrite.descriptorCount = 1;
    depthPyramidWrite.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    depthPyramidWrite.pImageInfo = &depthPyramidInfo;
    vkUpdateDescriptorSets(deviceOwner_.device, 1, &depthPyramidWrite, 0,
                           nullptr);
}

void VulkanRenderer::Impl::ensureGpuVisibilityCapacity(
    FrameResources& frame, const std::size_t frameIndex,
    const std::size_t candidateCount,
    const std::size_t clusterInstanceCapacity) {
    bool descriptorsChanged = false;
    if (candidateCount > frame.candidateCapacity) {
        std::size_t capacity = std::max(frame.candidateCapacity, std::size_t{1});
        while (capacity < candidateCount) {
            if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
                capacity = candidateCount;
                break;
            }
            capacity *= 2U;
        }
        if (capacity >
            static_cast<std::size_t>(
                deviceOwner_.physicalDeviceProperties.limits.maxStorageBufferRange /
                sizeof(GpuCullCandidate))) {
            throw std::runtime_error(
                "GPU cull candidate storage exceeds Vulkan buffer limits");
        }
        Buffer replacement = createBuffer(
            sizeof(GpuCullCandidate) * capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const std::string frameName = "Frame " + std::to_string(frameIndex);
        setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(replacement.buffer),
                      frameName + " Cull Candidate Buffer");
        vmaSetAllocationName(deviceOwner_.allocator, replacement.allocation,
                             (frameName + " Cull Candidate Allocation").c_str());
        replacement.resourceId = resourceOwner_.registry.registerResource(
            GpuResourceKind::Buffer, frameName + " Cull Candidate Buffer",
            replacement.size);
        try {
            checkVk(vmaMapMemory(deviceOwner_.allocator, replacement.allocation,
                                 &replacement.mapped),
                    "vmaMapMemory cull candidates");
        } catch (...) {
            destroyBuffer(replacement);
            throw;
        }
        destroyBuffer(frame.cullCandidates);
        frame.cullCandidates = takeBuffer(replacement);
        frame.candidateCapacity = capacity;
        descriptorsChanged = true;
    }
    if (candidateCount > frame.instanceCapacity) {
        ensureSceneInstanceCapacity(frame, frameIndex, candidateCount);
        descriptorsChanged = true;
    }
    if (clusterInstanceCapacity > frame.visibleInstanceIndexCapacity) {
        std::size_t capacity =
            std::max(frame.visibleInstanceIndexCapacity, std::size_t{1});
        while (capacity < clusterInstanceCapacity) {
            if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
                capacity = clusterInstanceCapacity;
                break;
            }
            capacity *= 2U;
        }
        if (capacity >
            static_cast<std::size_t>(
                deviceOwner_.physicalDeviceProperties.limits
                    .maxStorageBufferRange /
                sizeof(std::uint32_t))) {
            throw std::runtime_error(
                "GPU visible instance index storage exceeds Vulkan limits");
        }
        Buffer replacement = createBuffer(
            sizeof(std::uint32_t) * capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const std::string frameName = "Frame " + std::to_string(frameIndex);
        setObjectName(VK_OBJECT_TYPE_BUFFER,
                      handleToUint64(replacement.buffer),
                      frameName + " Visible Instance Index Buffer");
        vmaSetAllocationName(
            deviceOwner_.allocator, replacement.allocation,
            (frameName + " Visible Instance Index Allocation").c_str());
        replacement.resourceId = resourceOwner_.registry.registerResource(
            GpuResourceKind::Buffer,
            frameName + " Visible Instance Index Buffer", replacement.size);
        destroyBuffer(frame.visibleInstanceIndices);
        frame.visibleInstanceIndices = takeBuffer(replacement);
        frame.visibleInstanceIndexCapacity = capacity;
        updateFrameVisibleInstanceDescriptor(frameIndex);
        descriptorsChanged = true;
    }
    frame.clusterInstanceCapacity = clusterInstanceCapacity;
    if (descriptorsChanged) {
        updateFrameCullDescriptors(frameIndex);
    }
}

void VulkanRenderer::Impl::ensureSceneInstanceCapacity(FrameResources& frame, const std::size_t frameIndex, const std::size_t requiredCapacity) {
    if (requiredCapacity <= frame.instanceCapacity) {
        return;
    }

    std::size_t newCapacity = std::max(frame.instanceCapacity, std::size_t{1});
    while (newCapacity < requiredCapacity) {
        if (newCapacity > std::numeric_limits<std::size_t>::max() / 2U) {
            newCapacity = requiredCapacity;
            break;
        }
        newCapacity *= 2U;
    }

    Buffer previousInstanceData = takeBuffer(frame.instanceData);
    const std::size_t previousCapacity = frame.instanceCapacity;
    try {
        createFrameInstanceDataBuffer(frame, frameIndex, newCapacity);
        updateFrameInstanceDataDescriptor(frameIndex);
    } catch (...) {
        destroyBuffer(frame.instanceData);
        frame.instanceData = takeBuffer(previousInstanceData);
        frame.instanceCapacity = previousCapacity;
        throw;
    }
    destroyBuffer(previousInstanceData);
    logger()->info("Grew frame {} scene instance capacity to {} items ({:.2f} MiB)", frameIndex, frame.instanceCapacity, bytesToMiB(frame.instanceData.size));
}

void VulkanRenderer::Impl::replaceFrameImageAvailableSemaphore(FrameResources& frame, const std::size_t frameIndex) {
    VkSemaphore replacement = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    checkVk(vkCreateSemaphore(deviceOwner_.device, &semaphoreInfo, nullptr, &replacement), "vkCreateSemaphore imageAvailable");
    setObjectName(VK_OBJECT_TYPE_SEMAPHORE, handleToUint64(replacement),
                  "Frame " + std::to_string(frameIndex) + " Image Available Semaphore");
    if (frame.imageAvailable != VK_NULL_HANDLE) {
        vkDestroySemaphore(deviceOwner_.device, frame.imageAvailable, nullptr);
    }
    frame.imageAvailable = replacement;
}

void VulkanRenderer::Impl::updateDepthPyramidDescriptors() const {
    const std::size_t mipCount =
        resourceOwner_.depthPyramidMipViews.size();
    if (mipCount == 0U || mipCount > kMaxDepthPyramidMipLevels) {
        throw std::runtime_error("Depth pyramid descriptor mip count is invalid");
    }
    for (std::size_t mip = 0; mip < mipCount; ++mip) {
        const VkImageView sourceView =
            mip == 0U ? resourceOwner_.depth.view
                      : resourceOwner_.depthPyramidMipViews[mip - 1U];
        const VkImageLayout sourceLayout =
            mip == 0U ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      : VK_IMAGE_LAYOUT_GENERAL;
        const VkDescriptorImageInfo source{
            resourceOwner_.depthReductionSamplerEnabled
                ? resourceOwner_.depthReductionSampler
                : resourceOwner_.linearSampler,
            sourceView, sourceLayout};
        const VkDescriptorImageInfo destination{
            VK_NULL_HANDLE, resourceOwner_.depthPyramidMipViews[mip],
            VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet =
            resourceOwner_.depthPyramidDescriptorSets[mip];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &source;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet =
            resourceOwner_.depthPyramidDescriptorSets[mip];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &destination;
        vkUpdateDescriptorSets(deviceOwner_.device,
                               static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    for (std::size_t frameIndex = 0;
         frameIndex < kMaxFramesInFlight; ++frameIndex) {
        if (indirectSceneDrawsEnabled_) {
            updateFrameCullDescriptors(frameIndex);
        }
        updateFrameLightingDescriptors(frameIndex);
    }
}

void VulkanRenderer::Impl::createDepthPyramidDescriptorSets() {
    std::array<VkDescriptorSetLayout, kMaxDepthPyramidMipLevels> layouts{};
    layouts.fill(resourceOwner_.depthPyramidSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = resourceOwner_.descriptorPool;
    allocInfo.descriptorSetCount =
        static_cast<std::uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();
    checkVk(vkAllocateDescriptorSets(
                deviceOwner_.device, &allocInfo,
                resourceOwner_.depthPyramidDescriptorSets.data()),
            "vkAllocateDescriptorSets depth pyramid");
    updateDepthPyramidDescriptors();
}

void VulkanRenderer::Impl::createFrameResources() {
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> sceneLayouts{};
    sceneLayouts.fill(resourceOwner_.sceneSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = resourceOwner_.descriptorPool;
    allocInfo.descriptorSetCount = static_cast<std::uint32_t>(sceneLayouts.size());
    allocInfo.pSetLayouts = sceneLayouts.data();
    std::array<std::uint32_t, kMaxFramesInFlight> variableDescriptorCounts{};
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescriptorInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    if (resourceOwner_.bindlessMaterialsEnabled) {
        variableDescriptorCounts.fill(
            static_cast<std::uint32_t>(resourceOwner_.materialTextures.size()));
        variableDescriptorInfo.descriptorSetCount =
            static_cast<std::uint32_t>(variableDescriptorCounts.size());
        variableDescriptorInfo.pDescriptorCounts = variableDescriptorCounts.data();
        allocInfo.pNext = &variableDescriptorInfo;
    }
    checkVk(vkAllocateDescriptorSets(deviceOwner_.device, &allocInfo, resourceOwner_.sceneDescriptorSets.data()), "vkAllocateDescriptorSets scene");
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> cullLayouts{};
    cullLayouts.fill(resourceOwner_.cullSetLayout);
    VkDescriptorSetAllocateInfo cullAllocInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cullAllocInfo.descriptorPool = resourceOwner_.descriptorPool;
    cullAllocInfo.descriptorSetCount =
        static_cast<std::uint32_t>(cullLayouts.size());
    cullAllocInfo.pSetLayouts = cullLayouts.data();
    checkVk(vkAllocateDescriptorSets(
                deviceOwner_.device, &cullAllocInfo,
                resourceOwner_.cullDescriptorSets.data()),
            "vkAllocateDescriptorSets scene cull");
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> lightingLayouts{};
    lightingLayouts.fill(resourceOwner_.lightingSetLayout);
    VkDescriptorSetAllocateInfo lightingAllocInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    lightingAllocInfo.descriptorPool = resourceOwner_.descriptorPool;
    lightingAllocInfo.descriptorSetCount =
        static_cast<std::uint32_t>(lightingLayouts.size());
    lightingAllocInfo.pSetLayouts = lightingLayouts.data();
    checkVk(vkAllocateDescriptorSets(
                deviceOwner_.device, &lightingAllocInfo,
                resourceOwner_.lightingDescriptorSets.data()),
            "vkAllocateDescriptorSets lighting");
    const auto samplerForRole = [&](const TextureRole role) {
        switch (role) {
        case TextureRole::Normal: return resourceOwner_.normalTextureSampler;
        case TextureRole::MetallicRoughness:
        case TextureRole::Occlusion: return resourceOwner_.ormTextureSampler;
        case TextureRole::BaseColor:
        case TextureRole::Emissive: return resourceOwner_.textureSampler;
        }
        return resourceOwner_.textureSampler;
    };
    std::vector<VkDescriptorImageInfo> materialTextureInfos;
    if (resourceOwner_.bindlessMaterialsEnabled) {
        materialTextureInfos.reserve(resourceOwner_.materialTextures.size());
        for (std::size_t index = 0; index < resourceOwner_.materialTextures.size(); ++index) {
            materialTextureInfos.push_back({
                samplerForRole(resourceOwner_.materialTextureRoles[index]),
                resourceOwner_.materialTextures[index].view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        }
    } else {
        materialTextureInfos.reserve(vulkan_renderer_detail::kMaterialTextureCount);
        constexpr std::array<TextureRole, vulkan_renderer_detail::kMaterialTextureCount>
            roles{TextureRole::BaseColor, TextureRole::Normal,
                  TextureRole::MetallicRoughness};
        for (std::size_t role = 0; role < roles.size(); ++role) {
            materialTextureInfos.push_back({
                samplerForRole(roles[role]),
                resourceOwner_.materialTextures.at(
                    resourceOwner_.referenceMaterialTextureIndices[role]).view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        }
    }


    const std::size_t initialInstanceCapacity = kInitialSceneInstanceCapacity;
    for (std::size_t i = 0; i < frameOwner_.frames.size(); ++i) {
        FrameResources& frame = frameOwner_.frames[i];
        const std::string frameName = "Frame " + std::to_string(i);
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, handleToUint64(resourceOwner_.sceneDescriptorSets[i]), frameName + " Scene Descriptor Set");
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                      handleToUint64(resourceOwner_.cullDescriptorSets[i]),
                      frameName + " Scene Cull Descriptor Set");
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                      handleToUint64(resourceOwner_.lightingDescriptorSets[i]),
                      frameName + " Lighting Descriptor Set");

        VkCommandPoolCreateInfo framePoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        framePoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        framePoolInfo.queueFamilyIndex = deviceOwner_.queueFamilies.graphics.value();
        checkVk(vkCreateCommandPool(deviceOwner_.device, &framePoolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool frame");
        setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(frame.commandPool), frameName + " Command Pool");

        VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdAlloc.commandPool = frame.commandPool;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(deviceOwner_.device, &cmdAlloc, &frame.commandBuffer), "vkAllocateCommandBuffers frame");
        setObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, handleToUint64(frame.commandBuffer), frameName + " Command Buffer");
        frame.sceneUniforms = createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(frame.sceneUniforms.buffer), frameName + " Scene Uniform Buffer");
        const std::string uniformAllocationName = frameName + " Scene Uniform Allocation";
        vmaSetAllocationName(deviceOwner_.allocator, frame.sceneUniforms.allocation, uniformAllocationName.c_str());
        frame.sceneUniforms.resourceId = resourceOwner_.registry.registerResource(GpuResourceKind::Buffer, uniformAllocationName, frame.sceneUniforms.size);
        checkVk(vmaMapMemory(deviceOwner_.allocator, frame.sceneUniforms.allocation, &frame.sceneUniforms.mapped), "vmaMapMemory scene uniforms");
        const auto createLightingBuffer =
            [&](Buffer& buffer, const VkDeviceSize size,
                const VkBufferUsageFlags usage,
                const VkMemoryPropertyFlags properties,
                const std::string& name, const bool map) {
                buffer = createBuffer(size, usage, properties);
                setObjectName(VK_OBJECT_TYPE_BUFFER,
                              handleToUint64(buffer.buffer), name);
                const std::string allocationName = name + " Allocation";
                vmaSetAllocationName(deviceOwner_.allocator, buffer.allocation,
                                     allocationName.c_str());
                buffer.resourceId = resourceOwner_.registry.registerResource(
                    GpuResourceKind::Buffer, name, buffer.size);
                if (map) {
                    checkVk(vmaMapMemory(deviceOwner_.allocator,
                                         buffer.allocation, &buffer.mapped),
                            ("vmaMapMemory " + name).c_str());
                }
            };
        createLightingBuffer(
            frame.localLights,
            sizeof(RenderLocalLight) * kMaximumLocalLights,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frameName + " Local Light Buffer", true);
        createLightingBuffer(
            frame.lightingUniforms, sizeof(GpuLightingUniforms),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frameName + " Lighting Uniform Buffer", true);
        const std::size_t initialLightTileColumns =
            (swapchainOwner_.extent.width + kLightTileSize - 1U) /
            kLightTileSize;
        const std::size_t initialLightTileRows =
            (swapchainOwner_.extent.height + kLightTileSize - 1U) /
            kLightTileSize;
        frame.lightTileCapacity =
            initialLightTileColumns * initialLightTileRows;
        createLightingBuffer(
            frame.lightTileHeaders,
            sizeof(LightTileHeader) * frame.lightTileCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            frameName + " Light Tile Header Buffer", false);
        createLightingBuffer(
            frame.lightTileIndices,
            sizeof(std::uint32_t) * kMaximumLightsPerTile *
                frame.lightTileCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            frameName + " Light Tile Index Buffer", false);
        createLightingBuffer(
            frame.lightListCounters, sizeof(GpuLightListCounters),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frameName + " Light List Counter Buffer", true);
        frame.shadowInstanceIndexCapacity =
            initialInstanceCapacity * kShadowAtlasSlotCount;
        createLightingBuffer(
            frame.shadowInstanceIndices,
            sizeof(std::uint32_t) * frame.shadowInstanceIndexCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frameName + " Shadow Instance Index Buffer", true);
        createLightingBuffer(
            frame.shadowIndirectCommands,
            sizeof(VkDrawIndexedIndirectCommand) *
                resourceOwner_.sceneMeshes.size() * kShadowAtlasSlotCount,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frameName + " Shadow Indirect Command Buffer", true);

        frame.visibleInstanceIndices = createBuffer(
            sizeof(std::uint32_t) * initialInstanceCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        frame.visibleInstanceIndexCapacity = initialInstanceCapacity;
        setObjectName(VK_OBJECT_TYPE_BUFFER,
                      handleToUint64(frame.visibleInstanceIndices.buffer),
                      frameName + " Visible Instance Index Buffer");
        vmaSetAllocationName(
            deviceOwner_.allocator, frame.visibleInstanceIndices.allocation,
            (frameName + " Visible Instance Index Allocation").c_str());
        frame.visibleInstanceIndices.resourceId =
            resourceOwner_.registry.registerResource(
                GpuResourceKind::Buffer,
                frameName + " Visible Instance Index Buffer",
                frame.visibleInstanceIndices.size);
        createFrameInstanceDataBuffer(frame, i, initialInstanceCapacity);
        frame.cullCandidates = createBuffer(
            sizeof(GpuCullCandidate) * initialInstanceCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        frame.candidateCapacity = initialInstanceCapacity;
        setObjectName(VK_OBJECT_TYPE_BUFFER,
                      handleToUint64(frame.cullCandidates.buffer),
                      frameName + " Cull Candidate Buffer");
        vmaSetAllocationName(deviceOwner_.allocator,
                             frame.cullCandidates.allocation,
                             (frameName + " Cull Candidate Allocation").c_str());
        frame.cullCandidates.resourceId = resourceOwner_.registry.registerResource(
            GpuResourceKind::Buffer, frameName + " Cull Candidate Buffer",
            frame.cullCandidates.size);
        checkVk(vmaMapMemory(deviceOwner_.allocator,
                             frame.cullCandidates.allocation,
                             &frame.cullCandidates.mapped),
                "vmaMapMemory cull candidates");
        frame.cullUniforms = createBuffer(
            sizeof(GpuCullUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        setObjectName(VK_OBJECT_TYPE_BUFFER,
                      handleToUint64(frame.cullUniforms.buffer),
                      frameName + " Cull Uniform Buffer");
        vmaSetAllocationName(deviceOwner_.allocator, frame.cullUniforms.allocation,
                             (frameName + " Cull Uniform Allocation").c_str());
        frame.cullUniforms.resourceId = resourceOwner_.registry.registerResource(
            GpuResourceKind::Buffer, frameName + " Cull Uniform Buffer",
            frame.cullUniforms.size);
        checkVk(vmaMapMemory(deviceOwner_.allocator, frame.cullUniforms.allocation,
                             &frame.cullUniforms.mapped),
                "vmaMapMemory cull uniforms");
        frame.cullCounters = createBuffer(
            sizeof(GpuCullCounters), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        setObjectName(VK_OBJECT_TYPE_BUFFER,
                      handleToUint64(frame.cullCounters.buffer),
                      frameName + " Cull Counter Buffer");
        vmaSetAllocationName(deviceOwner_.allocator,
                             frame.cullCounters.allocation,
                             (frameName + " Cull Counter Allocation").c_str());
        frame.cullCounters.resourceId = resourceOwner_.registry.registerResource(
            GpuResourceKind::Buffer, frameName + " Cull Counter Buffer",
            frame.cullCounters.size);
        checkVk(vmaMapMemory(deviceOwner_.allocator,
                             frame.cullCounters.allocation,
                             &frame.cullCounters.mapped),
                "vmaMapMemory cull counters");

        if (indirectSceneDrawsEnabled_) {
            frame.indirectCommands = createBuffer(
                sizeof(VkDrawIndexedIndirectCommand) *
                    resourceOwner_.sceneClusters.size(),
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(frame.indirectCommands.buffer), frameName + " Scene Indirect Commands Buffer");
            const std::string indirectAllocationName = frameName + " Scene Indirect Commands Allocation";
            vmaSetAllocationName(deviceOwner_.allocator, frame.indirectCommands.allocation, indirectAllocationName.c_str());
            frame.indirectCommands.resourceId = resourceOwner_.registry.registerResource(GpuResourceKind::Buffer, indirectAllocationName, frame.indirectCommands.size);
            checkVk(vmaMapMemory(deviceOwner_.allocator, frame.indirectCommands.allocation, &frame.indirectCommands.mapped), "vmaMapMemory indirect commands");
        }

        VkDescriptorBufferInfo sceneBufferInfo{};
        sceneBufferInfo.buffer = frame.sceneUniforms.buffer;
        sceneBufferInfo.offset = 0;
        sceneBufferInfo.range = sizeof(SceneUniforms);
        VkDescriptorBufferInfo instanceBufferInfo{};
        instanceBufferInfo.buffer = frame.instanceData.buffer;
        instanceBufferInfo.offset = 0;
        instanceBufferInfo.range = frame.instanceData.size;
        VkDescriptorBufferInfo visibleIndexBufferInfo{};
        visibleIndexBufferInfo.buffer = frame.visibleInstanceIndices.buffer;
        visibleIndexBufferInfo.offset = 0;
        visibleIndexBufferInfo.range = frame.visibleInstanceIndices.size;
        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = resourceOwner_.sceneDescriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &sceneBufferInfo;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = resourceOwner_.sceneDescriptorSets[i];
        writes[1].dstBinding = 3;
        writes[1].descriptorCount = static_cast<std::uint32_t>(materialTextureInfos.size());
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = materialTextureInfos.data();
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet = resourceOwner_.sceneDescriptorSets[i];
        writes[2].dstBinding = 1;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &instanceBufferInfo;
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[3].dstSet = resourceOwner_.sceneDescriptorSets[i];
        writes[3].dstBinding = 2;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &visibleIndexBufferInfo;
        vkUpdateDescriptorSets(deviceOwner_.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        if (indirectSceneDrawsEnabled_) {
            updateFrameCullDescriptors(i);
        }
        updateFrameLightingDescriptors(i);

        replaceFrameImageAvailableSemaphore(frame, i);

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        checkVk(vkCreateFence(deviceOwner_.device, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), frameName + " In-Flight Fence");
    }
    createDepthPyramidDescriptorSets();
}

void VulkanRenderer::Impl::createTimestampQueries() {
    if (!config_.gpuTimestamps) {
        frameOwner_.timestampsEnabled = false;
        frameOwner_.timestampValidBits = 0;
        deviceOwner_.info.timestampQueries = false;
        stats_.gpuTimestampsValid = false;
        return;
    }

    const QueueFamilies families = findQueueFamilies(deviceOwner_.physicalDevice);
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(deviceOwner_.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(deviceOwner_.physicalDevice, &queueFamilyCount, queueProperties.data());
    frameOwner_.timestampValidBits = queueProperties[families.graphics.value()].timestampValidBits;
    frameOwner_.timestampsEnabled = frameOwner_.timestampValidBits > 0U;
    deviceOwner_.info.timestampQueries = frameOwner_.timestampsEnabled;
    if (!frameOwner_.timestampsEnabled) {
        logger()->warn("Graphics queue does not support timestamp queries");
        return;
    }

    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = static_cast<std::uint32_t>(kMaxFramesInFlight) * kTimestampQueriesPerFrame;
    checkVk(vkCreateQueryPool(deviceOwner_.device, &queryInfo, nullptr, &frameOwner_.timestampQueryPool), "vkCreateQueryPool timestamps");
    setObjectName(VK_OBJECT_TYPE_QUERY_POOL, handleToUint64(frameOwner_.timestampQueryPool), "Frame Timestamp Query Pool");
}

void VulkanRenderer::Impl::createFrameGraph(const bool resizeRecompile) {
    const auto compileStart = std::chrono::steady_clock::now();
    const auto makeVariant = [&](const bool depthPrepass, const bool screenshot) {
        FrameGraphVariant variant{};
        FrameGraph& graph = variant.graph;
        const VkFormat depthFormat = findDepthFormat();
        const VkFormat hdrFormat = findHdrFormat();
        const FrameGraph::ResourceHandle depth = graph.addResource({
            "Depth Image", FrameGraphResourceKind::Image, false,
            imageByteEstimate(swapchainOwner_.extent, depthFormat), 1U, 1U});
        const FrameGraph::ResourceHandle hdr = graph.addResource({
            "HDR Color Image", FrameGraphResourceKind::Image, false,
            imageByteEstimate(swapchainOwner_.extent, hdrFormat), 1U, 2U});
        const FrameGraph::ResourceHandle swapchain = graph.addResource({
            "Swapchain Image", FrameGraphResourceKind::Image, true});
        FrameGraph::ResourceHandle cullCandidates{};
        FrameGraph::ResourceHandle cullCounters{};
        FrameGraph::ResourceHandle cullUniforms{};
        FrameGraph::ResourceHandle visibleInstances{};
        FrameGraph::ResourceHandle sceneInstances{};
        FrameGraph::ResourceHandle depthPyramid{};
        FrameGraph::ResourceHandle indirectCommands{};
        FrameGraph::ResourceHandle clusterData{};
        FrameGraph::ResourceHandle meshClusterRanges{};
        FrameGraph::PassHandle cullPass{};
        const FrameGraph::ResourceHandle localLights = graph.addResource({
            "Local Lights", FrameGraphResourceKind::Buffer, true});
        const FrameGraph::ResourceHandle lightingUniforms = graph.addResource({
            "Lighting Uniforms", FrameGraphResourceKind::Buffer, true});
        const FrameGraph::ResourceHandle lightTileHeaders = graph.addResource({
            "Light Tile Headers", FrameGraphResourceKind::Buffer, true});
        const FrameGraph::ResourceHandle lightTileIndices = graph.addResource({
            "Light Tile Indices", FrameGraphResourceKind::Buffer, true});
        const FrameGraph::ResourceHandle lightListCounters = graph.addResource({
            "Light List Counters", FrameGraphResourceKind::Buffer, true});
        const FrameGraph::ResourceHandle shadowAtlas = graph.addResource({
            "Shadow Atlas", FrameGraphResourceKind::Image, true});
        const FrameGraph::ResourceHandle shadowInstances = graph.addResource({
            "Shadow Instance Indices", FrameGraphResourceKind::Buffer, true});
        const FrameGraph::ResourceHandle shadowCommands = graph.addResource({
            "Shadow Indirect Commands", FrameGraphResourceKind::Buffer, true});
        if (indirectSceneDrawsEnabled_) {
            depthPyramid = graph.addResource({
                "Depth Pyramid", FrameGraphResourceKind::Image, true});
            cullCandidates = graph.addResource({
                "GPU Cull Candidates", FrameGraphResourceKind::Buffer, true});
            cullCounters = graph.addResource({
                "GPU Cull Counters", FrameGraphResourceKind::Buffer, true});
            cullUniforms = graph.addResource({
                "GPU Cull Uniforms", FrameGraphResourceKind::Buffer, true});
            visibleInstances = graph.addResource({
                "GPU Visible Instance Indices", FrameGraphResourceKind::Buffer, true});
            sceneInstances = graph.addResource({
                "GPU Scene Instances", FrameGraphResourceKind::Buffer, true});
            indirectCommands = graph.addResource({
                "GPU Indirect Commands", FrameGraphResourceKind::Buffer, true});
            clusterData = graph.addResource({
                "GPU Cluster Data", FrameGraphResourceKind::Buffer, true});
            meshClusterRanges = graph.addResource({
                "GPU Mesh Cluster Ranges", FrameGraphResourceKind::Buffer, true});
            cullPass = graph.addPass({
                "GPU Cluster Cull", {0.54f, 0.34f, 0.92f, 1.0f}});
            graph.read(cullPass, cullCandidates,
                       FrameGraphUsage::StorageBuffer);
            graph.read(cullPass, cullUniforms,
                       FrameGraphUsage::UniformBuffer);
            graph.read(cullPass, depthPyramid,
                       FrameGraphUsage::SampledImage);
            graph.read(cullPass, clusterData, FrameGraphUsage::StorageBuffer);
            graph.read(cullPass, meshClusterRanges,
                       FrameGraphUsage::StorageBuffer);
            graph.write(cullPass, visibleInstances,
                        FrameGraphUsage::StorageBuffer);
            graph.write(cullPass, indirectCommands,
                        FrameGraphUsage::StorageBuffer);
            graph.write(cullPass, cullCounters,
                        FrameGraphUsage::StorageBuffer);
            graph.setFinalUsage(cullCounters, FrameGraphUsage::HostRead);
        }
        FrameGraph::PassHandle shadowPass{};
        const bool shadowsEnabled =
            config_.shadows && indirectSceneDrawsEnabled_;
        if (shadowsEnabled) {
            shadowPass = graph.addPass({
                "Shadow Atlas", {0.32F, 0.30F, 0.28F, 1.0F}});
            graph.writeAttachment(
                shadowPass, shadowAtlas, FrameGraphUsage::DepthAttachment,
                FrameGraphAttachmentLoad::Clear,
                FrameGraphAttachmentStore::Store);
            graph.read(shadowPass, lightingUniforms,
                       FrameGraphUsage::UniformBuffer);
            graph.read(shadowPass, sceneInstances,
                       FrameGraphUsage::StorageBuffer);
            graph.read(shadowPass, shadowInstances,
                       FrameGraphUsage::StorageBuffer);
            graph.read(shadowPass, shadowCommands,
                       FrameGraphUsage::IndirectBuffer);
        }

        FrameGraph::PassHandle depthPass{};
        if (depthPrepass) {
            depthPass = graph.addPass({
                "Depth Prepass", {0.16f, 0.42f, 0.18f, 1.0f}});
            graph.writeAttachment(
                depthPass, depth, FrameGraphUsage::DepthAttachment,
                FrameGraphAttachmentLoad::Clear,
                FrameGraphAttachmentStore::Store);
            if (indirectSceneDrawsEnabled_) {
                graph.read(depthPass, visibleInstances,
                           FrameGraphUsage::StorageBuffer);
                graph.read(depthPass, sceneInstances,
                           FrameGraphUsage::StorageBuffer);
                graph.read(depthPass, indirectCommands,
                           FrameGraphUsage::IndirectBuffer);
            }
        }

        FrameGraph::PassHandle depthPyramidPass{};
        if (depthPrepass && indirectSceneDrawsEnabled_) {
            depthPyramidPass = graph.addPass({
                "Depth Pyramid Build", {0.28f, 0.72f, 0.88f, 1.0f}});
            graph.read(depthPyramidPass, depth,
                       FrameGraphUsage::SampledImage);
            graph.write(depthPyramidPass, depthPyramid,
                        FrameGraphUsage::StorageImage);
        }

        const FrameGraph::PassHandle lightAssignmentPass = graph.addPass({
            "Forward+ Light Assignment", {0.98F, 0.75F, 0.18F, 1.0F}});
        graph.read(lightAssignmentPass, localLights,
                   FrameGraphUsage::StorageBuffer);
        graph.read(lightAssignmentPass, lightingUniforms,
                   FrameGraphUsage::UniformBuffer);
        if (depthPrepass && indirectSceneDrawsEnabled_) {
            graph.read(lightAssignmentPass, depthPyramid,
                       FrameGraphUsage::SampledImage);
        }
        graph.write(lightAssignmentPass, lightTileHeaders,
                    FrameGraphUsage::StorageBuffer);
        graph.write(lightAssignmentPass, lightTileIndices,
                    FrameGraphUsage::StorageBuffer);
        graph.write(lightAssignmentPass, lightListCounters,
                    FrameGraphUsage::StorageBuffer);
        graph.setFinalUsage(lightListCounters, FrameGraphUsage::HostRead);

        const FrameGraph::PassHandle hdrPass = graph.addPass({
            "HDR Scene Pass", {0.18f, 0.32f, 0.95f, 1.0f}});
        if (depthPrepass) {
            graph.readAttachment(
                hdrPass, depth, FrameGraphUsage::DepthAttachment,
                FrameGraphAttachmentLoad::Load,
                FrameGraphAttachmentStore::Discard);
        } else {
            graph.writeAttachment(
                hdrPass, depth, FrameGraphUsage::DepthAttachment,
                FrameGraphAttachmentLoad::Clear,
                FrameGraphAttachmentStore::Discard);
        }
        graph.writeAttachment(
            hdrPass, hdr, FrameGraphUsage::ColorAttachment,
            FrameGraphAttachmentLoad::Clear,
            FrameGraphAttachmentStore::Store);
        if (indirectSceneDrawsEnabled_) {
            graph.read(hdrPass, visibleInstances,
                       FrameGraphUsage::StorageBuffer);
            graph.read(hdrPass, sceneInstances,
                       FrameGraphUsage::StorageBuffer);
            graph.read(hdrPass, indirectCommands,
                       FrameGraphUsage::IndirectBuffer);
        }
        graph.read(hdrPass, localLights, FrameGraphUsage::StorageBuffer);
        graph.read(hdrPass, lightingUniforms,
                   FrameGraphUsage::UniformBuffer);
        graph.read(hdrPass, lightTileHeaders,
                   FrameGraphUsage::StorageBuffer);
        graph.read(hdrPass, lightTileIndices,
                   FrameGraphUsage::StorageBuffer);
        if (shadowsEnabled) {
            graph.read(hdrPass, shadowAtlas, FrameGraphUsage::SampledImage);
        }

        if (!depthPrepass && indirectSceneDrawsEnabled_) {
            depthPyramidPass = graph.addPass({
                "Depth Pyramid Build", {0.28f, 0.72f, 0.88f, 1.0f}});
            graph.read(depthPyramidPass, depth,
                       FrameGraphUsage::SampledImage);
            graph.write(depthPyramidPass, depthPyramid,
                        FrameGraphUsage::StorageImage);
        }

        const FrameGraph::PassHandle tonemapPass = graph.addPass({
            config_.debugOverlay ? "Tonemap + ImGui Pass" : "Tonemap Pass",
            {0.95f, 0.58f, 0.16f, 1.0f}});
        graph.read(tonemapPass, hdr, FrameGraphUsage::SampledImage);
        graph.writeAttachment(tonemapPass, swapchain, FrameGraphUsage::ColorAttachment,
                              FrameGraphAttachmentLoad::Discard, FrameGraphAttachmentStore::Store);

        FrameGraph::PassHandle screenshotPass{};
        FrameGraph::ResourceHandle screenshotReadback{};
        if (screenshot) {
            screenshotPass = graph.addPass({"Screenshot Readback", {0.36f, 0.74f, 0.95f, 1.0f}});
            screenshotReadback = graph.addResource({
                "Screenshot Readback Buffer", FrameGraphResourceKind::Buffer, true});
            graph.read(screenshotPass, swapchain, FrameGraphUsage::TransferSource);
            graph.write(screenshotPass, screenshotReadback, FrameGraphUsage::TransferDestination);
            graph.setFinalUsage(screenshotReadback, FrameGraphUsage::HostRead);
        }
        graph.setFinalUsage(swapchain, FrameGraphUsage::Present);
        graph.compile();

        const bool depthEdgesMatch = depthPrepass
            ? graph.hasEdge(depthPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment)
                  && graph.hasEdge(hdrPass, depth, FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment)
            : graph.hasEdge(hdrPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment);
        if (!depthEdgesMatch) {
            throw std::runtime_error("FrameGraph depth variant edges are invalid");
        }
        if (screenshot &&
            (!graph.hasEdge(screenshotPass, swapchain, FrameGraphAccess::Read,
                            FrameGraphUsage::TransferSource) ||
             !graph.hasEdge(screenshotPass, screenshotReadback, FrameGraphAccess::Write,
                            FrameGraphUsage::TransferDestination))) {
            throw std::runtime_error("FrameGraph screenshot variant edges are missing");
        }

        variant.resources.depth = depth;
        variant.resources.hdr = hdr;
        variant.resources.swapchain = swapchain;
        variant.resources.cullCandidates = cullCandidates;
        variant.resources.cullCounters = cullCounters;
        variant.resources.cullUniforms = cullUniforms;
        variant.resources.visibleInstances = visibleInstances;
        variant.resources.sceneInstances = sceneInstances;
        variant.resources.indirectCommands = indirectCommands;
        variant.resources.clusterData = clusterData;
        variant.resources.meshClusterRanges = meshClusterRanges;
        variant.resources.depthPyramid = depthPyramid;
        variant.resources.screenshotReadback = screenshotReadback;
        variant.resources.localLights = localLights;
        variant.resources.lightingUniforms = lightingUniforms;
        variant.resources.lightTileHeaders = lightTileHeaders;
        variant.resources.lightTileIndices = lightTileIndices;
        variant.resources.lightListCounters = lightListCounters;
        variant.resources.shadowAtlas = shadowAtlas;
        variant.resources.shadowInstances = shadowInstances;
        variant.resources.shadowCommands = shadowCommands;
        variant.passes.depthPrepass = depthPass;
        variant.passes.hdrScene = hdrPass;
        variant.passes.tonemap = tonemapPass;
        variant.passes.gpuCull = cullPass;
        variant.passes.depthPyramid = depthPyramidPass;
        variant.passes.shadows = shadowPass;
        variant.passes.lightAssignment = lightAssignmentPass;
        variant.passes.screenshotReadback = screenshotPass;
        return variant;
    };

    const bool depthPrepassAvailable = FrameGraphVariantPolicy::depthVariantAvailable(config_.depthPrepassMode, true);
    const bool depthPrepassOffAvailable = FrameGraphVariantPolicy::depthVariantAvailable(config_.depthPrepassMode, false);
    decltype(graphOwner_.variants) replacements{};
    const auto cacheVariant = [&](const bool depthPrepass, const bool screenshot) {
        const std::size_t index = FrameGraphVariantPolicy::index(depthPrepass, screenshot);
        replacements[index] = makeVariant(depthPrepass, screenshot);
    };
    if (depthPrepassAvailable) {
        cacheVariant(true, false);
        cacheVariant(true, true);
    }
    if (depthPrepassOffAvailable) {
        cacheVariant(false, false);
        cacheVariant(false, true);
    }

    graphOwner_.variants = std::move(replacements);
    const std::size_t baseVariantIndex = (depthPrepassAvailable ? 1U : 0U) | 2U;
    const FrameGraph& diagnosticGraph = graphOwner_.variants[baseVariantIndex].graph;
    stats_.cpuGraphCompileMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - compileStart).count();
    stats_.graphPassCount = static_cast<unsigned>(diagnosticGraph.passCount());
    stats_.graphResourceCount = static_cast<unsigned>(diagnosticGraph.resourceCount());
    stats_.graphBarrierCount = static_cast<unsigned>(diagnosticGraph.barrierPlan().size());
    stats_.graphPhysicalAllocationCount = diagnosticGraph.transientStats().allocationCount;
    stats_.graphTransientRequestedBytes = diagnosticGraph.transientStats().requestedBytes;
    stats_.graphTransientAllocatedBytes = diagnosticGraph.transientStats().allocatedBytes;
    ++stats_.graphRecompileCount;
    stats_.graphLastCompileWasResize = resizeRecompile;
    logger()->info("Compiled frame graph variants (depth prepass {}): {} cached "
                 "topologies, {} resources",
                   depthPrepassModeName(config_.depthPrepassMode),
                   (depthPrepassAvailable ? 2U : 0U) + (depthPrepassOffAvailable ? 2U : 0U),
                   graphOwner_.variants[baseVariantIndex].graph.resourceCount());
}

void VulkanRenderer::Impl::updateUniforms(FrameResources& frame, const Camera& camera, const Mat4& viewProjection, const double elapsedSeconds) {
    const Vec3 position = camera.position();
    const Vec3 lightDirection = normalize(Vec3{-0.45f, -1.0f, -0.35f});
    const Vec3 forward = camera.forward();
  const Vec3 right = camera.right();
  const Vec3 up = normalize(cross(right, forward));
  const float tanHalfFov = std::tan(camera.verticalFov() * 0.5F);
  const SceneUniforms uniforms{
        viewProjection,
        {position.x, position.y, position.z, static_cast<float>(elapsedSeconds)},
        {lightDirection.x, lightDirection.y, lightDirection.z, 0.0f},
        {1.0f, 0.93f, 0.82f, 8.0f},
        {0.46f, 0.58f, 0.82f, 0.055f},
        {0.14f, 0.12f, 0.10f, 0.030f},
      {forward.x, forward.y, forward.z, tanHalfFov},
      {right.x, right.y, right.z, camera.aspect()},
      {up.x, up.y, up.z, config_.atmosphere ? 1.0F : 0.0F},
    };
    std::memcpy(frame.sceneUniforms.mapped, &uniforms, sizeof(uniforms));
}

void VulkanRenderer::Impl::restoreFrameFenceAfterSubmitFailure(FrameResources& frame, const std::size_t frameIndex, const VkResult submitResult) {
    VkFence replacement = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const VkResult fenceResult = vkCreateFence(deviceOwner_.device, &fenceInfo, nullptr, &replacement);
    if (fenceResult == VK_SUCCESS) {
        const VkFence oldFence = frame.inFlight;
        frame.inFlight = replacement;
        vulkan_renderer_detail::replaceFenceReferences(pipelineOwner_.retiredSets, oldFence, replacement);
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), "Frame " + std::to_string(frameIndex) + " In-Flight Fence");
        if (oldFence != VK_NULL_HANDLE) {
            vkDestroyFence(deviceOwner_.device, oldFence, nullptr);
        }
        logger()->error("vkQueueSubmit frame failed with VkResult {}; restored "
                    "frame {} fence to signaled before throwing",
                        static_cast<int>(submitResult),
                        frameIndex);
    } else {
        logger()->error(
        "vkQueueSubmit frame failed with VkResult {}; failed to restore frame "
        "{} fence to signaled state (vkCreateFence returned {})",
                        static_cast<int>(submitResult),
                        frameIndex,
                        static_cast<int>(fenceResult));
    }
}

void VulkanRenderer::Impl::readBackGpuTimestamp(const std::uint32_t frameIndex) {
    if (!frameOwner_.timestampsEnabled || !frameOwner_.frames[frameIndex].submittedOnce) {
        stats_.gpuTimestampsValid = false;
        return;
    }
    std::array<std::uint64_t, kTimestampQueriesPerFrame> timestamps{};
    const std::uint32_t queryBase = frameIndex * kTimestampQueriesPerFrame;
    const VkResult result = vkGetQueryPoolResults(deviceOwner_.device, frameOwner_.timestampQueryPool, queryBase, kTimestampQueriesPerFrame,
                                                  sizeof(timestamps), timestamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
        const auto deltaTicks = [this](const std::uint64_t begin, const std::uint64_t end) -> std::uint64_t {
            if (frameOwner_.timestampValidBits >= 64U) {
                return end - begin;
            }
            const std::uint64_t mask = (1ULL << frameOwner_.timestampValidBits) - 1ULL;
            return ((end & mask) - (begin & mask)) & mask;
        };
        const double tickToMs = static_cast<double>(deviceOwner_.physicalDeviceProperties.limits.timestampPeriod) / 1'000'000.0;
        const FrameResources& frame = frameOwner_.frames[frameIndex];
        if (indirectSceneDrawsEnabled_) {
            stats_.gpuCullMs = static_cast<double>(
                deltaTicks(timestamps[kTimestampFrameStart],
                           timestamps[kTimestampCullEnd])) *
                tickToMs;
            stats_.gpuShadowMs = static_cast<double>(
                deltaTicks(timestamps[kTimestampCullEnd],
                           timestamps[kTimestampShadowEnd])) *
                tickToMs;
        } else {
            stats_.gpuCullMs = 0.0;
            stats_.gpuShadowMs = static_cast<double>(
                deltaTicks(timestamps[kTimestampFrameStart],
                           timestamps[kTimestampShadowEnd])) *
                tickToMs;
        }
        if (frame.submittedDepthPrepass) {
            stats_.gpuDepthPrepassMs = static_cast<double>(
                deltaTicks(timestamps[kTimestampShadowEnd],
                           timestamps[kTimestampDepthEnd])) *
                tickToMs;
            const std::uint32_t lightBegin = indirectSceneDrawsEnabled_
                ? kTimestampDepthPyramidEnd : kTimestampDepthEnd;
            stats_.gpuDepthPyramidMs = indirectSceneDrawsEnabled_
                ? static_cast<double>(
                      deltaTicks(timestamps[kTimestampDepthEnd],
                                 timestamps[kTimestampDepthPyramidEnd])) *
                      tickToMs
                : 0.0;
            stats_.gpuLightAssignmentMs = static_cast<double>(
                deltaTicks(timestamps[lightBegin],
                           timestamps[kTimestampLightAssignmentEnd])) *
                tickToMs;
        } else {
            stats_.gpuDepthPrepassMs = 0.0;
            stats_.gpuLightAssignmentMs = static_cast<double>(
                deltaTicks(timestamps[kTimestampShadowEnd],
                           timestamps[kTimestampLightAssignmentEnd])) *
                tickToMs;
            stats_.gpuDepthPyramidMs = indirectSceneDrawsEnabled_
                ? static_cast<double>(
                      deltaTicks(timestamps[kTimestampHdrEnd],
                                 timestamps[kTimestampDepthPyramidEnd])) *
                      tickToMs
                : 0.0;
        }
        stats_.gpuHdrSceneMs = static_cast<double>(
            deltaTicks(timestamps[kTimestampLightAssignmentEnd],
                       timestamps[kTimestampHdrEnd])) *
            tickToMs;
        const std::uint32_t finalBegin =
            !frame.submittedDepthPrepass && indirectSceneDrawsEnabled_
                ? kTimestampDepthPyramidEnd : kTimestampHdrEnd;
        stats_.gpuFinalPassMs = static_cast<double>(
            deltaTicks(timestamps[finalBegin],
                       timestamps[kTimestampFinalEnd])) *
            tickToMs;
        stats_.gpuFrameMs = static_cast<double>(deltaTicks(timestamps[kTimestampFrameStart], timestamps[kTimestampFinalEnd])) * tickToMs;
        stats_.gpuTimestampsValid = true;
    } else {
        stats_.gpuTimestampsValid = false;
    }
}

} // namespace ve
