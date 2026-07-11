#include "renderer/vulkan/VulkanRendererImpl.hpp"
#include <cmath>

namespace ve {

void VulkanRenderer::Impl::recoverAcquiredFrame(FrameResources& frame, const std::size_t frameIndex,
                                                const std::uint32_t imageIndex,
                                                const FrameImageSyncSnapshot& imageSyncSnapshot) {
    restoreFrameImageSyncState(imageIndex, imageSyncSnapshot);
    acquireRecoveryFailed_ = true;
    checkVk(vkDeviceWaitIdle(deviceOwner_.device), "vkDeviceWaitIdle recover acquired frame");
    replaceFrameImageAvailableSemaphore(frame, frameIndex);
    const VkExtent2D extent = window_.framebufferExtent();
    if (extent.width == 0U || extent.height == 0U) {
        cleanupSwapchain();
    } else {
        recreateSwapchain();
    }
    acquireRecoveryFailed_ = false;
}

void VulkanRenderer::Impl::draw(const Camera& camera, const SceneRenderList& renderItems, const double sceneBuildMs,
                                 const double elapsedSeconds, const double frameDeltaMs) {
    const auto throwOnValidationErrors = [this] {
        const std::uint64_t errorCount =
            deviceOwner_.validationMessages.errorCount.load(std::memory_order_relaxed);
        if (config_.requireValidation && errorCount > 0U) {
            throw std::runtime_error(
                "Strict Vulkan validation observed " + std::to_string(errorCount) +
                " error message(s); inspect the Vulkan validation log above");
        }
    };
    throwOnValidationErrors();
    if (!std::isfinite(sceneBuildMs) || sceneBuildMs < 0.0) {
        throw std::runtime_error("Scene build duration must be finite and non-negative");
    }
    if (acquireRecoveryFailed_) {
        throw std::runtime_error("Renderer acquire recovery previously failed; refusing semaphore reuse");
    }
    if (swapchainOwner_.handle == VK_NULL_HANDLE) {
        recreateSwapchain();
        if (swapchainOwner_.handle == VK_NULL_HANDLE) {
            return;
        }
    }
    FrameResources& frame = frameOwner_.frames[frameOwner_.currentFrame];
    checkVk(vkWaitForFences(deviceOwner_.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences frame");
    retireDeferredPipelineSets();
    readBackGpuTimestamp(static_cast<std::uint32_t>(frameOwner_.currentFrame));
    destroyFrameUploadWaitSemaphores(frame);
    retireCompletedUploads();
    pollShaderHotReload(elapsedSeconds);

    const auto cpuStart = std::chrono::steady_clock::now();
    const auto cpuSceneEnd = cpuStart;
    const Mat4 projection = camera.projectionMatrix();
    const Mat4 viewProjection = projection * camera.viewMatrix();
    SceneVisibilityPlan visibility = planSceneVisibility(camera, projection, viewProjection, renderItems);
    ensureSceneInstanceCapacity(frame, frameOwner_.currentFrame, visibility.visibleItemCount);
    updateUniforms(frame, camera, viewProjection, elapsedSeconds);
    checkVk(vkResetCommandPool(deviceOwner_.device, frame.commandPool, 0), "vkResetCommandPool frame");
    const bool useDepthPrepass = resolveDepthPrepassForFrame(visibility);
    auto cpuPrepareEnd = std::chrono::steady_clock::now();
    auto cpuRecordEnd = cpuPrepareEnd;
    auto cpuEnd = cpuPrepareEnd;

    std::uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(deviceOwner_.device, swapchainOwner_.handle, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        checkVk(acquire, "vkAcquireNextImageKHR");
    }
    VkSemaphore renderFinished = swapchainOwner_.renderFinishedSemaphores[imageIndex];
    const FrameImageSyncSnapshot imageSyncSnapshot = captureFrameImageSyncState(imageIndex);
    FrameSubmissionProgress submissionProgress = FrameSubmissionProgress::ImageAcquired;
    bool screenshotThisFrame = false;
    VkExtent2D screenshotExtent{};
    VkFormat screenshotFormat = VK_FORMAT_UNDEFINED;
    std::filesystem::path screenshotPath;
    bool retryScreenshotRequest = false;
    bool injectedAcquireRecoverySmoke = false;
    try {
        if (auto pendingScreenshot = readback_.takeRequest()) {
            screenshotPath = std::move(*pendingScreenshot);
            retryScreenshotRequest = true;
        }
        if (retryScreenshotRequest) {
            if (!readback_.swapchainTransferSourceSupported()) {
                logger()->warn("Screenshot requested but swapchain images do not support TRANSFER_SRC usage");
                retryScreenshotRequest = false;
            } else if (!screenshotFormatSupported()) {
                logger()->warn("Screenshot requested but swapchain format {} is not BGRA8/RGBA8 UNORM",
                               static_cast<int>(swapchainOwner_.format));
                retryScreenshotRequest = false;
            } else {
                const VkDeviceSize screenshotBytes = static_cast<VkDeviceSize>(swapchainOwner_.extent.width) *
                                                     static_cast<VkDeviceSize>(swapchainOwner_.extent.height) * 4U;
                Buffer& screenshotReadback = readback_.buffer();
                if (screenshotReadback.buffer != VK_NULL_HANDLE &&
                    screenshotReadback.size < screenshotBytes) {
                    destroyBuffer(screenshotReadback);
                }
                if (screenshotReadback.buffer == VK_NULL_HANDLE) {
                    screenshotReadback = createBuffer(
                        screenshotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        false, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
                    setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(screenshotReadback.buffer),
                                  "Screenshot Readback Buffer");
                    vmaSetAllocationName(deviceOwner_.allocator, screenshotReadback.allocation,
                                         "Screenshot Readback Allocation");
                    screenshotReadback.resourceId = resourceOwner_.registry.registerResource(
                        GpuResourceKind::Buffer, "Screenshot Readback Buffer",
                        screenshotReadback.size);
                }
                screenshotThisFrame = true;
                screenshotExtent = swapchainOwner_.extent;
                screenshotFormat = swapchainOwner_.format;
            }
        }

        beginImGuiFrame(frameDeltaMs);
        cpuPrepareEnd = std::chrono::steady_clock::now();
        cpuRecordEnd = cpuPrepareEnd;
        cpuEnd = cpuPrepareEnd;
        if (acquireRecoverySmokeArmed_) {
            acquireRecoverySmokeArmed_ = false;
            injectedAcquireRecoverySmoke = true;
            throw std::runtime_error("Injected post-acquire failure for recovery smoke");
        }
        const std::size_t graphVariantIndex = FrameGraphVariantPolicy::index(useDepthPrepass, screenshotThisFrame);
        FrameGraphVariant& graphVariant = graphOwner_.variants[graphVariantIndex];
        if (!graphVariant.graph.compiled()) {
            throw std::runtime_error("Requested frame graph topology variant is unavailable");
        }
        recordCommandBuffer(frame, imageIndex, renderItems, visibility, useDepthPrepass,
                            screenshotThisFrame ? &readback_.buffer() : nullptr,
                            graphVariant);
        cpuRecordEnd = std::chrono::steady_clock::now();

        collectPendingUploadWaitSemaphores(frameOwner_.pendingUploadWaitSemaphores);
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const VkPipelineStageFlags imageAvailableStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (frameOwner_.pendingUploadWaitSemaphores.empty()) {
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &frame.imageAvailable;
            submitInfo.pWaitDstStageMask = &imageAvailableStage;
        } else {
            frameOwner_.submitWaitSemaphores.clear();
            frameOwner_.submitWaitSemaphores.reserve(frameOwner_.pendingUploadWaitSemaphores.size() + 1U);
            frameOwner_.submitWaitSemaphores.push_back(frame.imageAvailable);
            frameOwner_.submitWaitSemaphores.insert(frameOwner_.submitWaitSemaphores.end(), frameOwner_.pendingUploadWaitSemaphores.begin(), frameOwner_.pendingUploadWaitSemaphores.end());

            frameOwner_.submitWaitStages.clear();
            frameOwner_.submitWaitStages.reserve(frameOwner_.submitWaitSemaphores.size());
            frameOwner_.submitWaitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            frameOwner_.submitWaitStages.insert(frameOwner_.submitWaitStages.end(), frameOwner_.pendingUploadWaitSemaphores.size(), VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

            submitInfo.waitSemaphoreCount = static_cast<std::uint32_t>(frameOwner_.submitWaitSemaphores.size());
            submitInfo.pWaitSemaphores = frameOwner_.submitWaitSemaphores.data();
            submitInfo.pWaitDstStageMask = frameOwner_.submitWaitStages.data();
        }
        frame.uploadWaitSemaphores.reserve(frame.uploadWaitSemaphores.size() + frameOwner_.pendingUploadWaitSemaphores.size());
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
        checkVk(vkResetFences(deviceOwner_.device, 1, &frame.inFlight), "vkResetFences frame");
        const VkResult submitResult = vkQueueSubmit(deviceOwner_.graphicsQueue, 1, &submitInfo, frame.inFlight);
        if (submitResult != VK_SUCCESS) {
            restoreFrameFenceAfterSubmitFailure(frame, frameOwner_.currentFrame, submitResult);
            checkVk(submitResult, "vkQueueSubmit frame");
        }
        submissionProgress = FrameSubmissionProgress::CommandsSubmitted;
    } catch (...) {
        if (frameFailureRequiresAcquireRecovery(submissionProgress)) {
            if (retryScreenshotRequest && !screenshotPath.empty()) {
                readback_.retry(std::move(screenshotPath));
            }
            recoverAcquiredFrame(frame, frameOwner_.currentFrame, imageIndex, imageSyncSnapshot);
        }
        if (injectedAcquireRecoverySmoke) {
            logger()->info("Recovered injected post-acquire frame failure");
            return;
        }
        throw;
    }
    markUploadWaitSemaphoresQueued(frame);
    frame.submittedOnce = true;
    cpuEnd = std::chrono::steady_clock::now();


    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchainOwner_.handle;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult present = vkQueuePresentKHR(deviceOwner_.presentQueue, &presentInfo);
    if (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR) {
        swapchainOwner_.imageStates[imageIndex] = {};
    }
    if (screenshotThisFrame) {
        checkVk(vkWaitForFences(deviceOwner_.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences screenshot capture");
        writeScreenshotPpm(readback_.buffer(), screenshotExtent, screenshotFormat,
                           screenshotPath);
        logger()->info("Saved screenshot {}", screenshotPath.string());
    }
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR || window_.consumeFramebufferResized()) {
        recreateSwapchain();
    } else if (present != VK_SUCCESS) {
        checkVk(present, "vkQueuePresentKHR");
    }
    throwOnValidationErrors();


    stats_.cpuFrameMs = sceneBuildMs + std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count();
    stats_.cpuSceneBuildMs = sceneBuildMs;
    stats_.cpuPrepareMs = std::chrono::duration<double, std::milli>(cpuPrepareEnd - cpuSceneEnd).count();
    stats_.cpuCommandRecordMs = std::chrono::duration<double, std::milli>(cpuRecordEnd - cpuPrepareEnd).count();
    stats_.cpuQueueSubmitMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuRecordEnd).count();
    stats_.frameDeltaMs = frameDeltaMs;
    stats_.elapsedSeconds = elapsedSeconds;
    frameOwner_.currentFrame = (frameOwner_.currentFrame + 1U) % kMaxFramesInFlight;
}

bool VulkanRenderer::Impl::resolveDepthPrepassForFrame(const SceneVisibilityPlan& visibility) {
    switch (config_.depthPrepassMode) {
    case DepthPrepassMode::ForceOn:
        return true;
    case DepthPrepassMode::ForceOff:
        return false;
    case DepthPrepassMode::Auto: {
        constexpr std::uint64_t kEnableTriangleThreshold = 75000;
        constexpr std::uint64_t kDisableTriangleThreshold = 45000;
        constexpr std::uint32_t kEnableItemThreshold = 24;
        constexpr std::uint32_t kDisableItemThreshold = 12;
        const bool enabled = pipelineOwner_.autoDepthPrepassEnabled
            ? (visibility.sceneTriangleCount >= kDisableTriangleThreshold || visibility.visibleItemCount >= kDisableItemThreshold)
            : (visibility.sceneTriangleCount >= kEnableTriangleThreshold || visibility.visibleItemCount >= kEnableItemThreshold);
        if (enabled != pipelineOwner_.autoDepthPrepassEnabled) {
            logger()->info("Auto depth prepass {} (visible {}, triangles {})",
                           enabled ? "enabled" : "disabled", visibility.visibleItemCount, visibility.sceneTriangleCount);
        }
        pipelineOwner_.autoDepthPrepassEnabled = enabled;
        return enabled;
    }
    }
    return false;
}

void VulkanRenderer::Impl::recordCommandBuffer(FrameResources& frame, const std::uint32_t imageIndex, const SceneRenderList& renderItems, const SceneVisibilityPlan& visibility, const bool useDepthPrepass, const Buffer* screenshotReadback, FrameGraphVariant& graphVariant) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer frame");

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainOwner_.extent.width);
    viewport.height = static_cast<float>(swapchainOwner_.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, swapchainOwner_.extent};
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    if (frameOwner_.timestampsEnabled) {
        const std::uint32_t queryBase = static_cast<std::uint32_t>(frameOwner_.currentFrame) * kTimestampQueriesPerFrame;
        vkCmdResetQueryPool(frame.commandBuffer, frameOwner_.timestampQueryPool, queryBase, kTimestampQueriesPerFrame);
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, frameOwner_.timestampQueryPool, queryBase + kTimestampFrameStart);
    }


    auto& visibleSceneWork = visibleSceneWorkScratch_;
    auto* instanceData = static_cast<InstanceData*>(frame.instanceData.mapped);

    auto& meshBatches = meshBatchScratch_;
    auto& meshFirstInstances = meshFirstInstanceScratch_;
    if (meshBatches.size() != resourceOwner_.sceneMeshes.size()) {
        meshBatches.resize(resourceOwner_.sceneMeshes.size());
        meshFirstInstances.resize(resourceOwner_.sceneMeshes.size());
        materializedInstanceCountScratch_.resize(resourceOwner_.sceneMeshes.size());
        instanceSortScratch_.resize(resourceOwner_.sceneMeshes.size());
        instanceSortKeyScratch_.resize(resourceOwner_.sceneMeshes.size());
    }
    const SceneGridRange& gridRange = visibility.gridRange;
    const std::vector<SceneGridTile>& gridTiles = renderItems.materialGridTiles();
    const bool useGridTiles = visibility.useGridTiles;
    const bool gridVisibilityCacheHit = visibility.gridVisibilityCacheHit;
    const std::size_t gridWorkBegin = visibility.gridWorkBegin;
    const std::size_t gridWorkEnd = visibility.gridWorkEnd;
    std::uint32_t visibleInstanceCount = 0;
    for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
        meshFirstInstances[meshIndex] = visibleInstanceCount;
        visibleInstanceCount += visibility.meshInstanceCounts[meshIndex];
    }
    if (visibleInstanceCount > frame.instanceCapacity) {
        throw std::runtime_error("Scene visibility plan exceeds frame instance capacity");
    }
    const InstanceMaterializationPolicy materializationPolicy = instanceMaterializationPolicy(useDepthPrepass);
    const bool directMappedMaterialization = materializationPolicy == InstanceMaterializationPolicy::DirectMapped;
    auto& materializedInstanceCounts = materializedInstanceCountScratch_;
    std::ranges::fill(materializedInstanceCounts, 0U);
    if (!directMappedMaterialization) {
        for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
            std::vector<InstanceData>& instances = instanceSortScratch_[meshIndex];
            instances.clear();
            instances.reserve(visibility.meshInstanceCounts[meshIndex]);
            std::vector<InstanceSortKey>& sortKeys = instanceSortKeyScratch_[meshIndex];
            sortKeys.clear();
            sortKeys.reserve(visibility.meshInstanceCounts[meshIndex]);
        }
    }

    const auto makeInstanceData = [](const SceneRenderItem& item) {
        const std::array<Vec4, 3> normalMatrix = normalMatrixColumns(item.model);
        return InstanceData{item.model,
                            normalMatrix[0],
                            normalMatrix[1],
                            normalMatrix[2],
                            item.material.albedoRoughness,
                            item.material.emissiveMetallic,
                            item.material.flags};
    };
    const auto instanceDepth = [&](const InstanceData& instance) {
        const Vec3 position{instance.model.m[12], instance.model.m[13], instance.model.m[14]};
        return dot(position - visibility.cameraPosition, visibility.cameraForward);
    };
    const auto writeInstanceData = [&](const std::size_t meshIndex, const InstanceData& data) {
        const std::uint32_t outputIndex = materializedInstanceCounts[meshIndex]++;
        if (outputIndex >= visibility.meshInstanceCounts[meshIndex]) {
            throw std::runtime_error("Scene instance materialization exceeded visibility count");
        }
        if (directMappedMaterialization) {
            instanceData[meshFirstInstances[meshIndex] + outputIndex] = data;
            return;
        }
        std::vector<InstanceData>& instances = instanceSortScratch_[meshIndex];
        instanceSortKeyScratch_[meshIndex].push_back(InstanceSortKey{instanceDepth(data), outputIndex});
        instances.push_back(data);
    };
    const auto writeCachedGridInstances = [&](const std::size_t meshIndex) {
        for (const InstanceData& data : gridVisibilityCache_.instanceDataByMesh[meshIndex]) {
            writeInstanceData(meshIndex, data);
        }
    };
    const auto writeVisibleItem = [&](const std::size_t itemIndex, const std::size_t meshIndex, const bool cacheGridInstance) {
        const InstanceData data = makeInstanceData(renderItems[itemIndex]);
        writeInstanceData(meshIndex, data);
        if (cacheGridInstance) {
            gridVisibilityCache_.instanceDataByMesh[meshIndex].push_back(data);
        }
    };
    const auto materializeWorkRange = [&](const std::size_t begin, const std::size_t end, const bool cacheGridInstances) {
        for (std::size_t workIndex = begin; workIndex < end; ++workIndex) {
            const VisibleSceneWork& work = visibleSceneWork[workIndex];
            const std::size_t meshIndex = work.meshIndex;
            if (work.kind == VisibleSceneWork::Kind::Item) {
                writeVisibleItem(work.index, meshIndex, cacheGridInstances);
                continue;
            }
            const SceneGridTile& tile = gridTiles[work.index];
            for (std::uint32_t row = tile.rowBegin; row < tile.rowEnd; ++row) {
                const std::size_t rowBase = gridRange.firstItem + (static_cast<std::size_t>(row) * gridRange.columns);
                for (std::uint32_t column = tile.columnBegin; column < tile.columnEnd; ++column) {
                    writeVisibleItem(rowBase + column, meshIndex, cacheGridInstances);
                }
            }
        }
    };

    if (gridVisibilityCacheHit) {
        materializeWorkRange(0, gridWorkBegin, false);
        for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
            writeCachedGridInstances(meshIndex);
        }
        materializeWorkRange(gridWorkEnd, visibleSceneWork.size(), false);
    } else {
        materializeWorkRange(0, gridWorkBegin, false);
        materializeWorkRange(gridWorkBegin, gridWorkEnd, useGridTiles);
        materializeWorkRange(gridWorkEnd, visibleSceneWork.size(), false);
    }
    if (useGridTiles && !gridVisibilityCacheHit) {
        gridVisibilityCache_.valid = true;
    }
    for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
        if (materializedInstanceCounts[meshIndex] != visibility.meshInstanceCounts[meshIndex]) {
            throw std::runtime_error("Scene instance materialization did not match visibility counts");
        }
        if (directMappedMaterialization) {
            continue;
        }
        std::vector<InstanceData>& instances = instanceSortScratch_[meshIndex];
        std::vector<InstanceSortKey>& sortKeys = instanceSortKeyScratch_[meshIndex];
        if (instances.size() != visibility.meshInstanceCounts[meshIndex] ||
            sortKeys.size() != visibility.meshInstanceCounts[meshIndex]) {
            throw std::runtime_error("Scene instance sort scratch did not match visibility counts");
        }
        if (sortKeys.size() > 1U) {
            std::sort(sortKeys.begin(), sortKeys.end(), [](const InstanceSortKey& lhs, const InstanceSortKey& rhs) {
                return lhs.depth < rhs.depth;
            });
        }
        InstanceData* output = instanceData + meshFirstInstances[meshIndex];
        for (std::size_t outputIndex = 0; outputIndex < sortKeys.size(); ++outputIndex) {
            output[outputIndex] = instances[sortKeys[outputIndex].index];
        }
    }

    std::uint32_t sceneDrawCalls = 0;
    for (std::size_t meshIndex = 0; meshIndex < resourceOwner_.sceneMeshes.size(); ++meshIndex) {
        if (visibility.meshInstanceCounts[meshIndex] == 0U) {
            continue;
        }
        meshBatches[sceneDrawCalls++] = MeshBatch{&meshForBatch(meshIndex), meshFirstInstances[meshIndex], visibility.meshInstanceCounts[meshIndex]};
    }

    if (indirectSceneDrawsEnabled_ && sceneDrawCalls > 0U) {
        auto* indirectCommands = static_cast<VkDrawIndexedIndirectCommand*>(frame.indirectCommands.mapped);
        for (std::uint32_t batchIndex = 0; batchIndex < sceneDrawCalls; ++batchIndex) {
            const MeshBatch& batch = meshBatches[batchIndex];
            indirectCommands[batchIndex] = VkDrawIndexedIndirectCommand{
                batch.mesh->indexCount,
                batch.instanceCount,
                batch.mesh->firstIndex,
                batch.mesh->vertexOffset,
                batch.firstInstance,
            };
        }
    }

    if (useDepthPrepass && !FrameGraphVariantPolicy::depthVariantAvailable(config_.depthPrepassMode, true)) {
        throw std::runtime_error("Depth prepass selected without a compiled frame-graph pass");
    }
    const VkDescriptorSet sceneSet = resourceOwner_.sceneDescriptorSets[frameOwner_.currentFrame];
    const VkDeviceSize offset = 0;
    if (sceneDrawCalls > 0U) {
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &resourceOwner_.sceneVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(frame.commandBuffer, resourceOwner_.sceneIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineOwner_.sceneLayout,
                                0, 1, &sceneSet, 0, nullptr);
    }

    FrameGraphRecordContext graphContext{
        this,
        &frame,
        &graphVariant,
        meshBatches.data(),
        screenshotReadback,
        {},
        imageIndex,
        sceneDrawCalls,
        useDepthPrepass};
    const FrameGraph::ExecutionCallbacks callbacks{
        &graphContext,
        &createGraphResource,
        &transitionGraphResource,
        &executeGraphPass,
        &retireGraphResource};
    const FrameGraph::ExecutionResult execution =
        graphVariant.graph.execute(callbacks, graphVariant.executionState);
    if (!execution.success()) {
        if (graphContext.failure != nullptr) {
            std::rethrow_exception(graphContext.failure);
        }
        throw std::runtime_error("Frame graph execution callback failed");
    }
    if (frameOwner_.timestampsEnabled) {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, frameOwner_.timestampQueryPool,
                             static_cast<std::uint32_t>(frameOwner_.currentFrame) * kTimestampQueriesPerFrame + kTimestampFinalEnd);
    }

    checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer frame");
    const std::uint32_t scenePassCount = useDepthPrepass ? 2U : 1U;
    frame.submittedDepthPrepass = useDepthPrepass;
    frame.submittedScenePassCount = scenePassCount;
    stats_.depthPrepassEnabled = useDepthPrepass;
    stats_.scenePassCount = scenePassCount;
    stats_.graphPassCount = static_cast<unsigned>(graphVariant.graph.passCount());
    stats_.graphResourceCount = static_cast<unsigned>(graphVariant.graph.resourceCount());
    stats_.graphBarrierCount = static_cast<unsigned>(graphVariant.graph.barrierPlan().size());
    stats_.graphPhysicalAllocationCount = graphVariant.graph.transientStats().allocationCount;
    stats_.graphTransientRequestedBytes = graphVariant.graph.transientStats().requestedBytes;
    stats_.graphTransientAllocatedBytes = graphVariant.graph.transientStats().allocatedBytes;
    stats_.sceneItemCount = static_cast<unsigned>(renderItems.size());
    stats_.visibleItemCount = visibility.visibleItemCount;
    stats_.sceneInstanceCapacity = static_cast<unsigned>(frame.instanceCapacity);
    stats_.sceneInstanceBufferMiB = bytesToMiB(frame.instanceData.size);
    stats_.meshBatchCount = sceneDrawCalls;
    const std::uint32_t sceneDrawCommandCount = (indirectSceneDrawsEnabled_ && sceneDrawCalls > 0U) ? 1U : sceneDrawCalls;
    stats_.drawCalls = (sceneDrawCommandCount * scenePassCount) + 1U;
    stats_.culledItemCount = visibility.culledItemCount;
    stats_.gridTileCount = visibility.gridTileCount;
    stats_.gridTilesCulled = visibility.gridTilesCulled;
    stats_.gridTilesAccepted = visibility.gridTilesAccepted;
    stats_.gridTilesIntersected = visibility.gridTilesIntersected;
    stats_.sphereLodHighCount = visibility.meshInstanceCounts[sceneMeshBatchIndex(SceneMeshBatchId::SphereHigh)];
    stats_.sphereLodMediumCount = visibility.meshInstanceCounts[sceneMeshBatchIndex(SceneMeshBatchId::SphereMedium)];
    stats_.sphereLodLowCount = visibility.meshInstanceCounts[sceneMeshBatchIndex(SceneMeshBatchId::SphereLow)];
    stats_.gridVisibilityCacheHit = visibility.gridVisibilityCacheHit;
    stats_.gridVisibilityWorkItems = visibility.gridVisibilityWorkItems;
    stats_.indirectSceneDraws = indirectSceneDrawsEnabled_ && sceneDrawCalls > 0U;
    stats_.sceneTriangleCount = visibility.sceneTriangleCount;
    stats_.triangleCount = (visibility.sceneTriangleCount * scenePassCount) + 1ULL;
}

void VulkanRenderer::Impl::recordSceneBatches(const FrameGraphRecordContext& context) const {
    if (context.sceneDrawCalls == 0U) {
        return;
    }
    const FrameResources& frame = *context.frame;
    if (indirectSceneDrawsEnabled_) {
        vkCmdDrawIndexedIndirect(frame.commandBuffer, frame.indirectCommands.buffer, 0,
                                 context.sceneDrawCalls, sizeof(VkDrawIndexedIndirectCommand));
        return;
    }
    for (std::uint32_t batchIndex = 0; batchIndex < context.sceneDrawCalls; ++batchIndex) {
        const MeshBatch& batch = context.meshBatches[batchIndex];
        vkCmdDrawIndexed(frame.commandBuffer, batch.mesh->indexCount, batch.instanceCount,
                         batch.mesh->firstIndex, batch.mesh->vertexOffset, batch.firstInstance);
    }
}

void VulkanRenderer::Impl::applyGraphTransition(const FrameGraphRecordContext& context,
                                                const FrameGraph::BarrierIntent& intent) {
    const FrameGraphVariant& variant = *context.variant;
    const bool forceMemoryDependency =
        intent.hasPrevious && intent.previousAccess == FrameGraphAccess::Write;
    if (intent.resource.index == variant.resources.screenshotReadback.index) {
        if (context.screenshotReadback == nullptr ||
            context.screenshotReadback->buffer == VK_NULL_HANDLE) {
            throw std::runtime_error("Frame graph screenshot buffer transition has no binding");
        }
        const VulkanBufferSyncState state = vulkanBufferSyncState(intent.access, intent.usage);
        VulkanBufferSyncState& current = readback_.buffer().syncState;
        if (forceMemoryDependency || current.stage != state.stage || current.access != state.access) {
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = current.stage;
            barrier.srcAccessMask = current.access;
            barrier.dstStageMask = state.stage;
            barrier.dstAccessMask = state.access;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = context.screenshotReadback->buffer;
            barrier.offset = 0;
            barrier.size = context.screenshotReadback->size;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(context.frame->commandBuffer, &dependency);
            current = state;
        }
        return;
    }

    const ImageSyncState state = imageSyncStateFor(intent.access, intent.usage);
    if (intent.resource.index == variant.resources.depth.index) {
        transitionImageTracked(context.frame->commandBuffer, resourceOwner_.depth.image, resourceOwner_.depth.syncState, state,
                               VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, forceMemoryDependency);
        return;
    }
    if (intent.resource.index == variant.resources.hdr.index) {
        transitionImageTracked(context.frame->commandBuffer, resourceOwner_.hdr.image, resourceOwner_.hdr.syncState, state,
                               VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, forceMemoryDependency);
        return;
    }
    if (intent.resource.index == variant.resources.swapchain.index) {
        transitionImageTracked(context.frame->commandBuffer, swapchainOwner_.images[context.imageIndex],
                               swapchainOwner_.imageStates[context.imageIndex], state, VK_IMAGE_ASPECT_COLOR_BIT,
                               0, 1, forceMemoryDependency);
        return;
    }
    throw std::runtime_error("Frame graph transition targets an unbound Vulkan resource");
}

void VulkanRenderer::Impl::recordDepthGraphPass(const FrameGraphRecordContext& context,
                                                const FrameGraph::PassDesc& desc,
                                                const FrameGraph::PassResources& resources) {
    const FrameGraph::Edge& edge = resources.edge(context.variant->resources.depth);
    const DebugLabelScope label{*this, context.frame->commandBuffer, desc.name, desc.debugColor};
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = resourceOwner_.depth.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    attachment.loadOp = edge.load == FrameGraphAttachmentLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
        : edge.load == FrameGraphAttachmentLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                       : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = edge.store == FrameGraphAttachmentStore::Store
        ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = swapchainOwner_.extent;
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &attachment;
    vkCmdBeginRendering(context.frame->commandBuffer, &rendering);
    vkCmdBindPipeline(context.frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineOwner_.depthPrepass);
    recordSceneBatches(context);
    vkCmdEndRendering(context.frame->commandBuffer);
    if (frameOwner_.timestampsEnabled) {
        vkCmdWriteTimestamp2(context.frame->commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             frameOwner_.timestampQueryPool,
                             static_cast<std::uint32_t>(frameOwner_.currentFrame) * kTimestampQueriesPerFrame +
                                 kTimestampDepthEnd);
    }
}

void VulkanRenderer::Impl::recordHdrGraphPass(const FrameGraphRecordContext& context,
                                              const FrameGraph::PassDesc& desc,
                                              const FrameGraph::PassResources& resources) {
    if (frameOwner_.timestampsEnabled && !context.useDepthPrepass) {
        vkCmdWriteTimestamp2(context.frame->commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             frameOwner_.timestampQueryPool,
                             static_cast<std::uint32_t>(frameOwner_.currentFrame) * kTimestampQueriesPerFrame +
                                 kTimestampDepthEnd);
    }
    const FrameGraph::Edge& colorEdge = resources.edge(context.variant->resources.hdr);
    const FrameGraph::Edge& depthEdge = resources.edge(context.variant->resources.depth);
    const DebugLabelScope label{*this, context.frame->commandBuffer, desc.name, desc.debugColor};
    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = resourceOwner_.hdr.view;
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = colorEdge.load == FrameGraphAttachmentLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
        : colorEdge.load == FrameGraphAttachmentLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                            : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = colorEdge.store == FrameGraphAttachmentStore::Store
        ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.clearValue.color = {{0.02f, 0.025f, 0.035f, 1.0f}};

    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = resourceOwner_.depth.view;
    depthAttachment.imageLayout = depthAttachmentLayout(depthEdge.access);
    depthAttachment.loadOp = depthEdge.load == FrameGraphAttachmentLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
        : depthEdge.load == FrameGraphAttachmentLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                            : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.storeOp = depthEdge.store == FrameGraphAttachmentStore::Store
        ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {0.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = swapchainOwner_.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAttachment;
    rendering.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(context.frame->commandBuffer, &rendering);
    vkCmdBindPipeline(context.frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      context.useDepthPrepass ? pipelineOwner_.scene : pipelineOwner_.sceneNoPrepass);
    recordSceneBatches(context);
    vkCmdEndRendering(context.frame->commandBuffer);
    if (frameOwner_.timestampsEnabled) {
        vkCmdWriteTimestamp2(context.frame->commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             frameOwner_.timestampQueryPool,
                             static_cast<std::uint32_t>(frameOwner_.currentFrame) * kTimestampQueriesPerFrame +
                                 kTimestampHdrEnd);
    }
}

void VulkanRenderer::Impl::recordTonemapGraphPass(const FrameGraphRecordContext& context,
                                                  const FrameGraph::PassDesc& desc,
                                                  const FrameGraph::PassResources& resources) {
    static_cast<void>(resources.edge(context.variant->resources.hdr));
    const FrameGraph::Edge& edge = resources.edge(context.variant->resources.swapchain);
    const DebugLabelScope label{*this, context.frame->commandBuffer, desc.name, desc.debugColor};
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = swapchainOwner_.imageViews[context.imageIndex];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = edge.load == FrameGraphAttachmentLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
        : edge.load == FrameGraphAttachmentLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                       : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = edge.store == FrameGraphAttachmentStore::Store
        ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = swapchainOwner_.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(context.frame->commandBuffer, &rendering);
    vkCmdBindPipeline(context.frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineOwner_.tonemap);
    vkCmdBindDescriptorSets(context.frame->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineOwner_.tonemapLayout, 0, 1, &resourceOwner_.tonemapDescriptorSet, 0, nullptr);
    const TonemapPushConstants pushConstants{
        config_.exposure, isSrgbSwapchainFormat(swapchainOwner_.format) ? 0U : 1U};
    vkCmdPushConstants(context.frame->commandBuffer, pipelineOwner_.tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pushConstants), &pushConstants);
    vkCmdDraw(context.frame->commandBuffer, 3, 1, 0, 0);
    renderImGui(context.frame->commandBuffer);
    vkCmdEndRendering(context.frame->commandBuffer);
}

void VulkanRenderer::Impl::recordScreenshotGraphPass(const FrameGraphRecordContext& context,
                                                     const FrameGraph::PassDesc& desc,
                                                     const FrameGraph::PassResources& resources) {
    static_cast<void>(resources.edge(context.variant->resources.swapchain));
    static_cast<void>(resources.edge(context.variant->resources.screenshotReadback));
    if (context.screenshotReadback == nullptr) {
        throw std::runtime_error("Screenshot frame-graph pass has no readback buffer");
    }
    const DebugLabelScope label{*this, context.frame->commandBuffer, desc.name, desc.debugColor};
    recordScreenshotCopy(context.frame->commandBuffer, context.imageIndex, *context.screenshotReadback);
}

bool VulkanRenderer::Impl::createGraphResource(void* rawContext, const FrameGraph::ResourceHandle resource,
                                               const FrameGraph::ResourceDesc& desc,
                                               const FrameGraph::TransientAllocation& allocation) noexcept {
    auto& context = *static_cast<FrameGraphRecordContext*>(rawContext);
    try {
        const FrameGraphResources& resources = context.variant->resources;
        const ImageResource* image = nullptr;
        if (resource.index == resources.depth.index) {
            image = &context.renderer->resourceOwner_.depth;
        } else if (resource.index == resources.hdr.index) {
            image = &context.renderer->resourceOwner_.hdr;
        } else {
            throw std::runtime_error("Frame graph attempted to create an imported or unbound resource");
        }
        if (desc.kind != FrameGraphResourceKind::Image || image->image == VK_NULL_HANDLE ||
            desc.transientBytes == 0U || image->allocationBytes < desc.transientBytes ||
            allocation.capacityBytes < desc.transientBytes) {
            throw std::runtime_error("Frame graph transient image binding is invalid");
        }
        return true;
    } catch (...) {
        context.failure = std::current_exception();
        return false;
    }
}

bool VulkanRenderer::Impl::transitionGraphResource(void* rawContext,
                                                   const FrameGraph::BarrierIntent& intent) noexcept {
    auto& context = *static_cast<FrameGraphRecordContext*>(rawContext);
    try {
        context.renderer->applyGraphTransition(context, intent);
        return true;
    } catch (...) {
        context.failure = std::current_exception();
        return false;
    }
}

bool VulkanRenderer::Impl::executeGraphPass(void* rawContext, const FrameGraph::PassHandle pass,
                                            const FrameGraph::PassDesc& desc,
                                            const FrameGraph::PassResources& resources) noexcept {
    auto& context = *static_cast<FrameGraphRecordContext*>(rawContext);
    try {
        const FrameGraphPasses& passes = context.variant->passes;
        if (pass.index == passes.depthPrepass.index) {
            context.renderer->recordDepthGraphPass(context, desc, resources);
        } else if (pass.index == passes.hdrScene.index) {
            context.renderer->recordHdrGraphPass(context, desc, resources);
        } else if (pass.index == passes.tonemap.index) {
            context.renderer->recordTonemapGraphPass(context, desc, resources);
        } else if (pass.index == passes.screenshotReadback.index) {
            context.renderer->recordScreenshotGraphPass(context, desc, resources);
        } else {
            throw std::runtime_error("Frame graph dispatched an unknown Vulkan pass");
        }
        return true;
    } catch (...) {
        context.failure = std::current_exception();
        return false;
    }
}

bool VulkanRenderer::Impl::retireGraphResource(void* rawContext, const FrameGraph::ResourceHandle resource,
                                               const FrameGraph::ResourceDesc&,
                                               const FrameGraph::TransientAllocation&) noexcept {
    const auto& context = *static_cast<const FrameGraphRecordContext*>(rawContext);
    const FrameGraphResources& resources = context.variant->resources;
    return resource.index == resources.depth.index || resource.index == resources.hdr.index;
}

} // namespace ve
