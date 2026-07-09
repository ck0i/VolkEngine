#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

VkDeviceSize VulkanRenderer::Impl::checkedSceneInstanceBufferSize(std::size_t capacity) const {
    if (capacity == 0U) {
        capacity = 1U;
    }
    if (capacity > static_cast<std::size_t>(std::numeric_limits<VkDeviceSize>::max() / sizeof(InstanceData))) {
        throw std::runtime_error("Scene instance capacity exceeds Vulkan buffer size range");
    }

    const VkDeviceSize instanceBufferSize = static_cast<VkDeviceSize>(sizeof(InstanceData) * capacity);
    if (instanceBufferSize > physicalDeviceProperties_.limits.maxStorageBufferRange) {
        throw std::runtime_error("Scene instance storage exceeds VkPhysicalDeviceLimits::maxStorageBufferRange");
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
        vmaSetAllocationName(allocator_, instanceData.allocation, instanceAllocationName.c_str());
        instanceData.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, instanceAllocationName, instanceData.size);
        checkVk(vmaMapMemory(allocator_, instanceData.allocation, &instanceData.mapped), "vmaMapMemory instance data");
    } catch (...) {
        destroyBuffer(instanceData);
        throw;
    }

    frame.instanceData = takeBuffer(instanceData);
    frame.instanceCapacity = capacity;
}

void VulkanRenderer::Impl::updateFrameInstanceDataDescriptor(const std::size_t frameIndex) const {
    const FrameResources& frame = frames_[frameIndex];
    VkDescriptorBufferInfo instanceBufferInfo{};
    instanceBufferInfo.buffer = frame.instanceData.buffer;
    instanceBufferInfo.offset = 0;
    instanceBufferInfo.range = frame.instanceData.size;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sceneDescriptorSets_[frameIndex];
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &instanceBufferInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
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

void VulkanRenderer::Impl::createFrameResources() {
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> sceneLayouts{};
    sceneLayouts.fill(sceneSetLayout_);
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = static_cast<std::uint32_t>(sceneLayouts.size());
    allocInfo.pSetLayouts = sceneLayouts.data();
    checkVk(vkAllocateDescriptorSets(device_, &allocInfo, sceneDescriptorSets_.data()), "vkAllocateDescriptorSets scene");

    const std::size_t initialInstanceCapacity = kInitialSceneInstanceCapacity;
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        FrameResources& frame = frames_[i];
        const std::string frameName = "Frame " + std::to_string(i);
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, handleToUint64(sceneDescriptorSets_[i]), frameName + " Scene Descriptor Set");

        VkCommandPoolCreateInfo framePoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        framePoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        framePoolInfo.queueFamilyIndex = queueFamilies_.graphics.value();
        checkVk(vkCreateCommandPool(device_, &framePoolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool frame");
        setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(frame.commandPool), frameName + " Command Pool");

        VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdAlloc.commandPool = frame.commandPool;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(device_, &cmdAlloc, &frame.commandBuffer), "vkAllocateCommandBuffers frame");
        setObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, handleToUint64(frame.commandBuffer), frameName + " Command Buffer");
        frame.sceneUniforms = createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(frame.sceneUniforms.buffer), frameName + " Scene Uniform Buffer");
        const std::string uniformAllocationName = frameName + " Scene Uniform Allocation";
        vmaSetAllocationName(allocator_, frame.sceneUniforms.allocation, uniformAllocationName.c_str());
        frame.sceneUniforms.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, uniformAllocationName, frame.sceneUniforms.size);
        checkVk(vmaMapMemory(allocator_, frame.sceneUniforms.allocation, &frame.sceneUniforms.mapped), "vmaMapMemory scene uniforms");

        createFrameInstanceDataBuffer(frame, i, initialInstanceCapacity);

        if (indirectSceneDrawsEnabled_) {
            frame.indirectCommands = createBuffer(sizeof(VkDrawIndexedIndirectCommand) * kSceneMeshBatchOrder.size(),
                                                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(frame.indirectCommands.buffer), frameName + " Scene Indirect Commands Buffer");
            const std::string indirectAllocationName = frameName + " Scene Indirect Commands Allocation";
            vmaSetAllocationName(allocator_, frame.indirectCommands.allocation, indirectAllocationName.c_str());
            frame.indirectCommands.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, indirectAllocationName, frame.indirectCommands.size);
            checkVk(vmaMapMemory(allocator_, frame.indirectCommands.allocation, &frame.indirectCommands.mapped), "vmaMapMemory indirect commands");
        }

        VkDescriptorBufferInfo sceneBufferInfo{};
        sceneBufferInfo.buffer = frame.sceneUniforms.buffer;
        sceneBufferInfo.offset = 0;
        sceneBufferInfo.range = sizeof(SceneUniforms);
        VkDescriptorBufferInfo instanceBufferInfo{};
        instanceBufferInfo.buffer = frame.instanceData.buffer;
        instanceBufferInfo.offset = 0;
        instanceBufferInfo.range = frame.instanceData.size;
        std::array<VkDescriptorImageInfo, vulkan_renderer_detail::kMaterialTextureCount> materialTextureInfos{};
        materialTextureInfos[0].sampler = textureSampler_;
        materialTextureInfos[0].imageView = groundAlbedoTexture_.view;
        materialTextureInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        materialTextureInfos[1].sampler = normalTextureSampler_;
        materialTextureInfos[1].imageView = groundNormalTexture_.view;
        materialTextureInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        materialTextureInfos[2].sampler = textureSampler_;
        materialTextureInfos[2].imageView = groundOrmTexture_.view;
        materialTextureInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = sceneDescriptorSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &sceneBufferInfo;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = sceneDescriptorSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = static_cast<std::uint32_t>(materialTextureInfos.size());
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = materialTextureInfos.data();
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet = sceneDescriptorSets_[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &instanceBufferInfo;
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore imageAvailable");
        setObjectName(VK_OBJECT_TYPE_SEMAPHORE, handleToUint64(frame.imageAvailable), frameName + " Image Available Semaphore");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), frameName + " In-Flight Fence");
    }
}

void VulkanRenderer::Impl::createTimestampQueries() {
    if (!config_.gpuTimestamps) {
        timestampsEnabled_ = false;
        timestampValidBits_ = 0;
        deviceInfo_.timestampQueries = false;
        stats_.gpuTimestampsValid = false;
        return;
    }

    const QueueFamilies families = findQueueFamilies(physicalDevice_);
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueProperties.data());
    timestampValidBits_ = queueProperties[families.graphics.value()].timestampValidBits;
    timestampsEnabled_ = timestampValidBits_ > 0U;
    deviceInfo_.timestampQueries = timestampsEnabled_;
    if (!timestampsEnabled_) {
        logger()->warn("Graphics queue does not support timestamp queries");
        return;
    }

    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = static_cast<std::uint32_t>(kMaxFramesInFlight) * kTimestampQueriesPerFrame;
    checkVk(vkCreateQueryPool(device_, &queryInfo, nullptr, &timestampQueryPool_), "vkCreateQueryPool timestamps");
    setObjectName(VK_OBJECT_TYPE_QUERY_POOL, handleToUint64(timestampQueryPool_), "Frame Timestamp Query Pool");
}

void VulkanRenderer::Impl::createFrameGraph() {
    frameGraph_ = {};

    const FrameGraph::ResourceHandle depth = frameGraph_.addResource({"Depth Image", FrameGraphResourceKind::Image, false});
    const FrameGraph::ResourceHandle hdr = frameGraph_.addResource({"HDR Color Image", FrameGraphResourceKind::Image, false});
    const FrameGraph::ResourceHandle swapchain = frameGraph_.addResource({"Swapchain Image", FrameGraphResourceKind::Image, true});

    const bool graphIncludesDepthPrepass = depthPrepassGraphIncludesPrepass(config_.depthPrepassMode);
    const bool graphAllowsDepthPrepassOff = config_.depthPrepassMode != DepthPrepassMode::ForceOn;
    FrameGraph::PassHandle depthPass{};
    if (graphIncludesDepthPrepass) {
        depthPass = frameGraph_.addPass({"Depth Prepass", {0.16f, 0.42f, 0.18f, 1.0f}});
        frameGraph_.write(depthPass, depth, FrameGraphUsage::DepthAttachment);
    }

    const FrameGraph::PassHandle hdrPass = frameGraph_.addPass({"HDR Scene Pass", {0.18f, 0.32f, 0.95f, 1.0f}});
    if (graphIncludesDepthPrepass) {
        frameGraph_.read(hdrPass, depth, FrameGraphUsage::DepthAttachment);
    }
    if (graphAllowsDepthPrepassOff) {
        frameGraph_.write(hdrPass, depth, FrameGraphUsage::DepthAttachment);
    }
    frameGraph_.write(hdrPass, hdr, FrameGraphUsage::ColorAttachment);

    const FrameGraph::PassHandle tonemapPass = frameGraph_.addPass({config_.debugOverlay ? "Tonemap + ImGui Pass" : "Tonemap Pass", {0.95f, 0.58f, 0.16f, 1.0f}});
    frameGraph_.read(tonemapPass, hdr, FrameGraphUsage::SampledImage);
    frameGraph_.write(tonemapPass, swapchain, FrameGraphUsage::ColorAttachment);
    const FrameGraph::PassHandle screenshotPass = frameGraph_.addPass({"Screenshot Readback", {0.36f, 0.74f, 0.95f, 1.0f}});
    frameGraph_.read(screenshotPass, swapchain, FrameGraphUsage::TransferSource);
    frameGraph_.setFinalUsage(swapchain, FrameGraphUsage::Present);

    frameGraph_.compile();
    const bool depthEdgesMatch = graphIncludesDepthPrepass
        ? frameGraph_.hasEdge(depthPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment)
              && frameGraph_.hasEdge(hdrPass, depth, FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment)
              && (!graphAllowsDepthPrepassOff || frameGraph_.hasEdge(hdrPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment))
        : frameGraph_.hasEdge(hdrPass, depth, FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment);
    if (!depthEdgesMatch) {
        throw std::runtime_error("FrameGraph depth edges do not match configured prepass mode");
    }
    if (!frameGraph_.hasEdge(screenshotPass, swapchain, FrameGraphAccess::Read, FrameGraphUsage::TransferSource)) {
        throw std::runtime_error("FrameGraph screenshot readback edge is missing");
    }
    frameGraphResources_ = {depth, hdr, swapchain};
    frameGraphPasses_ = {depthPass, hdrPass, tonemapPass, screenshotPass};
    logger()->info("Compiled frame graph (depth prepass {}): {} passes, {} resources, {} edges",
                   depthPrepassModeName(config_.depthPrepassMode), frameGraph_.passCount(), frameGraph_.resourceCount(), frameGraph_.edgeCount());
}

void VulkanRenderer::Impl::updateUniforms(FrameResources& frame, const Camera& camera, const Mat4& viewProjection, const double elapsedSeconds) {
    const Vec3 position = camera.position();
    const Vec3 lightDirection = normalize(Vec3{-0.45f, -1.0f, -0.35f});
    const SceneUniforms uniforms{
        viewProjection,
        {position.x, position.y, position.z, static_cast<float>(elapsedSeconds)},
        {lightDirection.x, lightDirection.y, lightDirection.z, 0.0f},
        {1.0f, 0.93f, 0.82f, 8.0f},
        {0.46f, 0.58f, 0.82f, 0.055f},
        {0.14f, 0.12f, 0.10f, 0.030f},
    };
    std::memcpy(frame.sceneUniforms.mapped, &uniforms, sizeof(uniforms));
}

void VulkanRenderer::Impl::restoreFrameFenceAfterSubmitFailure(FrameResources& frame, const std::size_t frameIndex, const VkResult submitResult) {
    VkFence replacement = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const VkResult fenceResult = vkCreateFence(device_, &fenceInfo, nullptr, &replacement);
    if (fenceResult == VK_SUCCESS) {
        const VkFence oldFence = frame.inFlight;
        frame.inFlight = replacement;
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), "Frame " + std::to_string(frameIndex) + " In-Flight Fence");
        if (oldFence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, oldFence, nullptr);
        }
        logger()->error("vkQueueSubmit frame failed with VkResult {}; restored frame {} fence to signaled before throwing",
                        static_cast<int>(submitResult),
                        frameIndex);
    } else {
        logger()->error("vkQueueSubmit frame failed with VkResult {}; failed to restore frame {} fence to signaled state (vkCreateFence returned {})",
                        static_cast<int>(submitResult),
                        frameIndex,
                        static_cast<int>(fenceResult));
    }
}

void VulkanRenderer::Impl::readBackGpuTimestamp(const std::uint32_t frameIndex) {
    if (!timestampsEnabled_ || !frames_[frameIndex].submittedOnce) {
        stats_.gpuTimestampsValid = false;
        return;
    }
    std::array<std::uint64_t, kTimestampQueriesPerFrame> timestamps{};
    const std::uint32_t queryBase = frameIndex * kTimestampQueriesPerFrame;
    const VkResult result = vkGetQueryPoolResults(device_, timestampQueryPool_, queryBase, kTimestampQueriesPerFrame,
                                                  sizeof(timestamps), timestamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_SUCCESS) {
        const auto deltaTicks = [this](const std::uint64_t begin, const std::uint64_t end) -> std::uint64_t {
            if (timestampValidBits_ >= 64U) {
                return end - begin;
            }
            const std::uint64_t mask = (1ULL << timestampValidBits_) - 1ULL;
            return ((end & mask) - (begin & mask)) & mask;
        };
        const double tickToMs = static_cast<double>(physicalDeviceProperties_.limits.timestampPeriod) / 1'000'000.0;
        if (frames_[frameIndex].submittedDepthPrepass) {
            stats_.gpuDepthPrepassMs = static_cast<double>(deltaTicks(timestamps[kTimestampFrameStart], timestamps[kTimestampDepthEnd])) * tickToMs;
            stats_.gpuHdrSceneMs = static_cast<double>(deltaTicks(timestamps[kTimestampDepthEnd], timestamps[kTimestampHdrEnd])) * tickToMs;
        } else {
            stats_.gpuDepthPrepassMs = 0.0;
            stats_.gpuHdrSceneMs = static_cast<double>(deltaTicks(timestamps[kTimestampFrameStart], timestamps[kTimestampHdrEnd])) * tickToMs;
        }
        stats_.gpuFinalPassMs = static_cast<double>(deltaTicks(timestamps[kTimestampHdrEnd], timestamps[kTimestampFinalEnd])) * tickToMs;
        stats_.gpuFrameMs = static_cast<double>(deltaTicks(timestamps[kTimestampFrameStart], timestamps[kTimestampFinalEnd])) * tickToMs;
        stats_.gpuTimestampsValid = true;
    } else {
        stats_.gpuTimestampsValid = false;
    }
}

} // namespace ve
