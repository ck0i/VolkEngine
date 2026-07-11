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
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &instanceBufferInfo;
    vkUpdateDescriptorSets(deviceOwner_.device, 1, &write, 0, nullptr);
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

void VulkanRenderer::Impl::createFrameResources() {
    std::array<VkDescriptorSetLayout, kMaxFramesInFlight> sceneLayouts{};
    sceneLayouts.fill(resourceOwner_.sceneSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = resourceOwner_.descriptorPool;
    allocInfo.descriptorSetCount = static_cast<std::uint32_t>(sceneLayouts.size());
    allocInfo.pSetLayouts = sceneLayouts.data();
    checkVk(vkAllocateDescriptorSets(deviceOwner_.device, &allocInfo, resourceOwner_.sceneDescriptorSets.data()), "vkAllocateDescriptorSets scene");

    const std::size_t initialInstanceCapacity = kInitialSceneInstanceCapacity;
    for (std::size_t i = 0; i < frameOwner_.frames.size(); ++i) {
        FrameResources& frame = frameOwner_.frames[i];
        const std::string frameName = "Frame " + std::to_string(i);
        setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, handleToUint64(resourceOwner_.sceneDescriptorSets[i]), frameName + " Scene Descriptor Set");

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

        createFrameInstanceDataBuffer(frame, i, initialInstanceCapacity);

        if (indirectSceneDrawsEnabled_) {
            frame.indirectCommands = createBuffer(sizeof(VkDrawIndexedIndirectCommand) * resourceOwner_.sceneMeshes.size(),
                                                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
        std::array<VkDescriptorImageInfo, vulkan_renderer_detail::kMaterialTextureCount> materialTextureInfos{};
        materialTextureInfos[0].sampler = resourceOwner_.textureSampler;
        materialTextureInfos[0].imageView =
            resourceOwner_.materialTextures.at(resourceOwner_.referenceMaterialTextureIndices[0]).view;
        materialTextureInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        materialTextureInfos[1].sampler = resourceOwner_.normalTextureSampler;
        materialTextureInfos[1].imageView =
            resourceOwner_.materialTextures.at(resourceOwner_.referenceMaterialTextureIndices[1]).view;
        materialTextureInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        materialTextureInfos[2].sampler = resourceOwner_.ormTextureSampler;
        materialTextureInfos[2].imageView =
            resourceOwner_.materialTextures.at(resourceOwner_.referenceMaterialTextureIndices[2]).view;
        materialTextureInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = resourceOwner_.sceneDescriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &sceneBufferInfo;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = resourceOwner_.sceneDescriptorSets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = static_cast<std::uint32_t>(materialTextureInfos.size());
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = materialTextureInfos.data();
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet = resourceOwner_.sceneDescriptorSets[i];
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &instanceBufferInfo;
        vkUpdateDescriptorSets(deviceOwner_.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        replaceFrameImageAvailableSemaphore(frame, i);

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        checkVk(vkCreateFence(deviceOwner_.device, &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), frameName + " In-Flight Fence");
    }
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
        const FrameGraph::ResourceHandle depth = graph.addResource({
            "Depth Image", FrameGraphResourceKind::Image, false,
            imageByteEstimate(swapchainOwner_.extent, depthFormat), 1U, 1U});
        const FrameGraph::ResourceHandle hdr = graph.addResource({
            "HDR Color Image", FrameGraphResourceKind::Image, false,
            imageByteEstimate(swapchainOwner_.extent, VK_FORMAT_R16G16B16A16_SFLOAT), 1U, 2U});
        const FrameGraph::ResourceHandle swapchain = graph.addResource({
            "Swapchain Image", FrameGraphResourceKind::Image, true});

        FrameGraph::PassHandle depthPass{};
        if (depthPrepass) {
            depthPass = graph.addPass({"Depth Prepass", {0.16f, 0.42f, 0.18f, 1.0f}});
            graph.writeAttachment(depthPass, depth, FrameGraphUsage::DepthAttachment,
                                  FrameGraphAttachmentLoad::Clear, FrameGraphAttachmentStore::Store);
        }

        const FrameGraph::PassHandle hdrPass = graph.addPass({"HDR Scene Pass", {0.18f, 0.32f, 0.95f, 1.0f}});
        if (depthPrepass) {
            graph.readAttachment(hdrPass, depth, FrameGraphUsage::DepthAttachment,
                                 FrameGraphAttachmentLoad::Load, FrameGraphAttachmentStore::Discard);
        } else {
            graph.writeAttachment(hdrPass, depth, FrameGraphUsage::DepthAttachment,
                                  FrameGraphAttachmentLoad::Clear, FrameGraphAttachmentStore::Discard);
        }
        graph.writeAttachment(hdrPass, hdr, FrameGraphUsage::ColorAttachment,
                              FrameGraphAttachmentLoad::Clear, FrameGraphAttachmentStore::Store);

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

        variant.resources = {depth, hdr, swapchain, screenshotReadback};
        variant.passes = {depthPass, hdrPass, tonemapPass, screenshotPass};
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
    logger()->info("Compiled frame graph variants (depth prepass {}): {} cached topologies, {} resources",
                   depthPrepassModeName(config_.depthPrepassMode),
                   (depthPrepassAvailable ? 2U : 0U) + (depthPrepassOffAvailable ? 2U : 0U),
                   graphOwner_.variants[baseVariantIndex].graph.resourceCount());
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
    const VkResult fenceResult = vkCreateFence(deviceOwner_.device, &fenceInfo, nullptr, &replacement);
    if (fenceResult == VK_SUCCESS) {
        const VkFence oldFence = frame.inFlight;
        frame.inFlight = replacement;
        vulkan_renderer_detail::replaceFenceReferences(pipelineOwner_.retiredSets, oldFence, replacement);
        setObjectName(VK_OBJECT_TYPE_FENCE, handleToUint64(frame.inFlight), "Frame " + std::to_string(frameIndex) + " In-Flight Fence");
        if (oldFence != VK_NULL_HANDLE) {
            vkDestroyFence(deviceOwner_.device, oldFence, nullptr);
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
        if (frameOwner_.frames[frameIndex].submittedDepthPrepass) {
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
