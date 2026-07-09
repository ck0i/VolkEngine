#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

void VulkanRenderer::Impl::draw(const Camera& camera, const double elapsedSeconds, const double frameDeltaMs) {
    FrameResources& frame = frames_[frameIndex_];
    checkVk(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences frame");
    retireDeferredPipelineSets();
    readBackGpuTimestamp(static_cast<std::uint32_t>(frameIndex_));
    destroyFrameUploadWaitSemaphores(frame);
    retireCompletedUploads();
    pollShaderHotReload(elapsedSeconds);

    std::uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        checkVk(acquire, "vkAcquireNextImageKHR");
    }
    bool screenshotThisFrame = false;
    VkExtent2D screenshotExtent{};
    VkFormat screenshotFormat = VK_FORMAT_UNDEFINED;
    std::filesystem::path screenshotPath;
    bool screenshotRequested = false;
    {
        const std::scoped_lock lock{screenshotRequestMutex_};
        if (screenshotPending_) {
            screenshotPath = std::move(screenshotPath_);
            screenshotPath_.clear();
            screenshotPending_ = false;
            screenshotRequested = true;
        }
    }
    if (screenshotRequested) {
        if (!swapchainTransferSrcSupported_) {
            logger()->warn("Screenshot requested but swapchain images do not support TRANSFER_SRC usage");
        } else if (!screenshotFormatSupported()) {
            logger()->warn("Screenshot requested but swapchain format {} is not BGRA8/RGBA8 UNORM", static_cast<int>(swapchainFormat_));
        } else {
            const VkDeviceSize screenshotBytes = static_cast<VkDeviceSize>(swapchainExtent_.width) * static_cast<VkDeviceSize>(swapchainExtent_.height) * 4U;
            if (screenshotReadback_.buffer != VK_NULL_HANDLE && screenshotReadback_.size != screenshotBytes) {
                destroyBuffer(screenshotReadback_);
            }
            if (screenshotReadback_.buffer == VK_NULL_HANDLE) {
                screenshotReadback_ = createBuffer(screenshotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                   false, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
                setObjectName(VK_OBJECT_TYPE_BUFFER, handleToUint64(screenshotReadback_.buffer), "Screenshot Readback Buffer");
                vmaSetAllocationName(allocator_, screenshotReadback_.allocation, "Screenshot Readback Allocation");
                screenshotReadback_.resourceId = resourceRegistry_.registerResource(GpuResourceKind::Buffer, "Screenshot Readback Buffer", screenshotReadback_.size);
            }
            screenshotThisFrame = true;
            screenshotExtent = swapchainExtent_;
            screenshotFormat = swapchainFormat_;
        }
    }

    const auto cpuStart = std::chrono::steady_clock::now();
    const SceneRenderList& renderItems = sceneRenderer_.build(elapsedSeconds, config_.materialGridRows, config_.materialGridColumns, config_.materialGridTileRows, config_.materialGridTileColumns);
    const auto cpuSceneEnd = std::chrono::steady_clock::now();
    const Mat4 projection = camera.projectionMatrix();
    const Mat4 viewProjection = projection * camera.viewMatrix();
    SceneVisibilityPlan visibility = planSceneVisibility(camera, projection, viewProjection, renderItems);
    ensureSceneInstanceCapacity(frame, frameIndex_, visibility.visibleItemCount);
    updateUniforms(frame, camera, viewProjection, elapsedSeconds);
    checkVk(vkResetCommandPool(device_, frame.commandPool, 0), "vkResetCommandPool frame");
    beginImGuiFrame(frameDeltaMs);
    const auto cpuPrepareEnd = std::chrono::steady_clock::now();
    auto cpuRecordEnd = cpuPrepareEnd;
    auto cpuEnd = cpuPrepareEnd;
    pendingUploadWaitSemaphores_.clear();
    VkSemaphore renderFinished = swapchainRenderFinishedSemaphores_[imageIndex];
    const FrameImageSyncSnapshot imageSyncSnapshot = captureFrameImageSyncState(imageIndex);
    bool frameCommandsSubmitted = false;
    try {
        recordCommandBuffer(frame, imageIndex, renderItems, visibility, screenshotThisFrame ? &screenshotReadback_ : nullptr);
        cpuRecordEnd = std::chrono::steady_clock::now();

        collectPendingUploadWaitSemaphores(pendingUploadWaitSemaphores_);
        submitWaitSemaphores_.clear();
        submitWaitSemaphores_.reserve(pendingUploadWaitSemaphores_.size() + 1U);
        submitWaitSemaphores_.push_back(frame.imageAvailable);
        submitWaitSemaphores_.insert(submitWaitSemaphores_.end(), pendingUploadWaitSemaphores_.begin(), pendingUploadWaitSemaphores_.end());

        submitWaitStages_.clear();
        submitWaitStages_.reserve(submitWaitSemaphores_.size());
        submitWaitStages_.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        submitWaitStages_.insert(submitWaitStages_.end(), pendingUploadWaitSemaphores_.size(), VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.waitSemaphoreCount = static_cast<std::uint32_t>(submitWaitSemaphores_.size());
        submitInfo.pWaitSemaphores = submitWaitSemaphores_.data();
        submitInfo.pWaitDstStageMask = submitWaitStages_.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
        checkVk(vkResetFences(device_, 1, &frame.inFlight), "vkResetFences frame");
        const VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frame.inFlight);
        if (submitResult != VK_SUCCESS) {
            restoreFrameFenceAfterSubmitFailure(frame, frameIndex_, submitResult);
            checkVk(submitResult, "vkQueueSubmit frame");
        }
        frameCommandsSubmitted = true;
    } catch (...) {
        if (!frameCommandsSubmitted) {
            restoreFrameImageSyncState(imageIndex, imageSyncSnapshot);
        }
        throw;
    }
    markUploadWaitSemaphoresQueued(frame, pendingUploadWaitSemaphores_);
    frame.submittedOnce = true;
    cpuEnd = std::chrono::steady_clock::now();


    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult present = vkQueuePresentKHR(presentQueue_, &presentInfo);
    if (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR) {
        swapchainStates_[imageIndex] = {};
    }
    if (screenshotThisFrame) {
        checkVk(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences screenshot capture");
        try {
            writeScreenshotPpm(screenshotReadback_, screenshotExtent, screenshotFormat, screenshotPath);
            logger()->info("Saved screenshot {}", screenshotPath.string());
            destroyBuffer(screenshotReadback_);
        } catch (...) {
            destroyBuffer(screenshotReadback_);
            throw;
        }
    }
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR || window_.consumeFramebufferResized()) {
        recreateSwapchain();
    } else if (present != VK_SUCCESS) {
        checkVk(present, "vkQueuePresentKHR");
    }


    stats_.cpuFrameMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count();
    stats_.cpuSceneBuildMs = std::chrono::duration<double, std::milli>(cpuSceneEnd - cpuStart).count();
    stats_.cpuPrepareMs = std::chrono::duration<double, std::milli>(cpuPrepareEnd - cpuSceneEnd).count();
    stats_.cpuCommandRecordMs = std::chrono::duration<double, std::milli>(cpuRecordEnd - cpuPrepareEnd).count();
    stats_.cpuQueueSubmitMs = std::chrono::duration<double, std::milli>(cpuEnd - cpuRecordEnd).count();
    stats_.frameDeltaMs = frameDeltaMs;
    stats_.elapsedSeconds = elapsedSeconds;
    frameIndex_ = (frameIndex_ + 1U) % kMaxFramesInFlight;
}

void VulkanRenderer::Impl::recordCommandBuffer(FrameResources& frame, const std::uint32_t imageIndex, const SceneRenderList& renderItems, const SceneVisibilityPlan& visibility, const Buffer* screenshotReadback) {
    static_assert(kSceneMeshBatchOrder.size() == kSceneMeshBatchCount);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer frame");

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, swapchainExtent_};
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    if (timestampsEnabled_) {
        const std::uint32_t queryBase = static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame;
        vkCmdResetQueryPool(frame.commandBuffer, timestampQueryPool_, queryBase, kTimestampQueriesPerFrame);
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestampQueryPool_, queryBase + kTimestampFrameStart);
    }


    auto& visibleSceneWork = visibleSceneWorkScratch_;
    auto* instanceData = static_cast<InstanceData*>(frame.instanceData.mapped);

    struct MeshBatch {
        const GpuMesh* mesh = nullptr;
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
    };
    std::array<MeshBatch, kSceneMeshBatchOrder.size()> meshBatches{};
    std::array<std::uint32_t, kSceneMeshBatchOrder.size()> meshFirstInstances{};
    std::array<std::uint32_t, kSceneMeshBatchOrder.size()> meshWriteCursors{};
    const SceneGridRange& gridRange = visibility.gridRange;
    const std::vector<SceneGridTile>& gridTiles = renderItems.materialGridTiles();
    const bool useGridTiles = visibility.useGridTiles;
    const bool gridVisibilityCacheHit = visibility.gridVisibilityCacheHit;
    const std::size_t gridWorkBegin = visibility.gridWorkBegin;
    const std::size_t gridWorkEnd = visibility.gridWorkEnd;
    std::uint32_t visibleInstanceCount = 0;
    for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
        meshFirstInstances[meshIndex] = visibleInstanceCount;
        meshWriteCursors[meshIndex] = visibleInstanceCount;
        visibleInstanceCount += visibility.meshInstanceCounts[meshIndex];
    }
    if (visibleInstanceCount > frame.instanceCapacity) {
        throw std::runtime_error("Scene visibility plan exceeds frame instance capacity");
    }

    const auto makeInstanceData = [](const SceneRenderItem& item) {
        return InstanceData{item.model, item.material.albedoRoughness, item.material.emissiveMetallic, item.material.flags};
    };
    const auto writeCachedGridInstances = [&](const std::size_t meshIndex) {
        const std::vector<InstanceData>& cachedInstances = gridVisibilityCache_.instanceDataByMesh[meshIndex];
        if (!cachedInstances.empty()) {
            std::memcpy(instanceData + meshWriteCursors[meshIndex], cachedInstances.data(), sizeof(InstanceData) * cachedInstances.size());
            meshWriteCursors[meshIndex] += static_cast<std::uint32_t>(cachedInstances.size());
        }
    };
    const auto writeVisibleItem = [&](const std::size_t itemIndex, const std::size_t meshIndex, const bool cacheGridInstance) {
        const InstanceData data = makeInstanceData(renderItems[itemIndex]);
        instanceData[meshWriteCursors[meshIndex]++] = data;
        if (cacheGridInstance) {
            gridVisibilityCache_.instanceDataByMesh[meshIndex].push_back(data);
        }
    };
    const auto materializeWorkRange = [&](const std::size_t begin, const std::size_t end) {
        for (std::size_t workIndex = begin; workIndex < end; ++workIndex) {
            const VisibleSceneWork& work = visibleSceneWork[workIndex];
            const std::size_t meshIndex = work.meshIndex;
            const bool cacheGridInstance = useGridTiles && !gridVisibilityCacheHit && workIndex >= gridWorkBegin && workIndex < gridWorkEnd;
            if (work.kind == VisibleSceneWork::Kind::Item) {
                writeVisibleItem(work.index, meshIndex, cacheGridInstance);
                continue;
            }
            const SceneGridTile& tile = gridTiles[work.index];
            for (std::uint32_t row = tile.rowBegin; row < tile.rowEnd; ++row) {
                const std::size_t rowBase = gridRange.firstItem + (static_cast<std::size_t>(row) * gridRange.columns);
                for (std::uint32_t column = tile.columnBegin; column < tile.columnEnd; ++column) {
                    writeVisibleItem(rowBase + column, meshIndex, cacheGridInstance);
                }
            }
        }
    };

    if (gridVisibilityCacheHit) {
        materializeWorkRange(0, gridWorkBegin);
        for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
            writeCachedGridInstances(meshIndex);
        }
        materializeWorkRange(gridWorkBegin, visibleSceneWork.size());
    } else {
        materializeWorkRange(0, visibleSceneWork.size());
    }
    if (useGridTiles && !gridVisibilityCacheHit) {
        gridVisibilityCache_.valid = true;
    }


    std::uint32_t sceneDrawCalls = 0;
    for (std::size_t meshIndex = 0; meshIndex < kSceneMeshBatchOrder.size(); ++meshIndex) {
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

    const VkDeviceSize offset = 0;
    const auto drawSceneBatches = [&] {
        if (sceneDrawCalls == 0U) {
            return;
        }
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &sceneVertexBuffer_.buffer, &offset);
        vkCmdBindIndexBuffer(frame.commandBuffer, sceneIndexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
        if (indirectSceneDrawsEnabled_) {
            vkCmdDrawIndexedIndirect(frame.commandBuffer, frame.indirectCommands.buffer, 0, sceneDrawCalls, sizeof(VkDrawIndexedIndirectCommand));
            return;
        }
        for (std::uint32_t batchIndex = 0; batchIndex < sceneDrawCalls; ++batchIndex) {
            const MeshBatch& batch = meshBatches[batchIndex];
            vkCmdDrawIndexed(frame.commandBuffer, batch.mesh->indexCount, batch.instanceCount,
                             batch.mesh->firstIndex, batch.mesh->vertexOffset, batch.firstInstance);
        }
    };

    const VkDescriptorSet sceneSet = sceneDescriptorSets_[frameIndex_];
    const bool useDepthPrepass = resolveDepthPrepass(config_.depthPrepassMode);
    const FrameGraph::PassDesc* depthPass = useDepthPrepass ? &frameGraph_.pass(frameGraphPasses_.depthPrepass) : nullptr;
    const FrameGraph::PassDesc& hdrPass = frameGraph_.pass(frameGraphPasses_.hdrScene);
    const FrameGraph::PassDesc& tonemapPass = frameGraph_.pass(frameGraphPasses_.tonemap);
    const FrameGraph::PassDesc& screenshotPass = frameGraph_.pass(frameGraphPasses_.screenshotReadback);


    if (useDepthPrepass) {
        {
            const DebugLabelScope depthLabel{*this, frame.commandBuffer, depthPass->name, depthPass->debugColor};
            transitionImageTracked(frame.commandBuffer, depth_.image, depth_.syncState,
                                   imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment),
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo depthPrepassAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depthPrepassAttachment.imageView = depth_.view;
            depthPrepassAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthPrepassAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthPrepassAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthPrepassAttachment.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo depthPrepassInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            depthPrepassInfo.renderArea.offset = {0, 0};
            depthPrepassInfo.renderArea.extent = swapchainExtent_;
            depthPrepassInfo.layerCount = 1;
            depthPrepassInfo.colorAttachmentCount = 0;
            depthPrepassInfo.pDepthAttachment = &depthPrepassAttachment;
            vkCmdBeginRendering(frame.commandBuffer, &depthPrepassInfo);
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPrepassPipeline_);
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, 0, 1, &sceneSet, 0, nullptr);
            drawSceneBatches();
            vkCmdEndRendering(frame.commandBuffer);
        }

        transitionImageTracked(frame.commandBuffer, depth_.image, depth_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::DepthAttachment),
                               VK_IMAGE_ASPECT_DEPTH_BIT);
    } else {
        transitionImageTracked(frame.commandBuffer, depth_.image, depth_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::DepthAttachment),
                               VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (timestampsEnabled_) {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueryPool_,
                             static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame + kTimestampDepthEnd);
    }

    {
        const DebugLabelScope hdrLabel{*this, frame.commandBuffer, hdrPass.name, hdrPass.debugColor};
        transitionImageTracked(frame.commandBuffer, hdr_.image, hdr_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment),
                               VK_IMAGE_ASPECT_COLOR_BIT);

        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = hdr_.view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{0.02f, 0.025f, 0.035f, 1.0f}};

        VkRenderingAttachmentInfo sceneDepthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sceneDepthAttachment.imageView = depth_.view;
        sceneDepthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sceneDepthAttachment.loadOp = useDepthPrepass ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        sceneDepthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        sceneDepthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderInfo.renderArea.offset = {0, 0};
        renderInfo.renderArea.extent = swapchainExtent_;
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;
        renderInfo.pDepthAttachment = &sceneDepthAttachment;
        vkCmdBeginRendering(frame.commandBuffer, &renderInfo);

        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, useDepthPrepass ? scenePipeline_ : sceneNoPrepassPipeline_);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, 0, 1, &sceneSet, 0, nullptr);
        drawSceneBatches();
        vkCmdEndRendering(frame.commandBuffer);

        transitionImageTracked(frame.commandBuffer, hdr_.image, hdr_.syncState,
                               imageSyncStateFor(FrameGraphAccess::Read, FrameGraphUsage::SampledImage),
                               VK_IMAGE_ASPECT_COLOR_BIT);
        if (timestampsEnabled_) {
            vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueryPool_,
                                 static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame + kTimestampHdrEnd);
        }
    }

    {
        const DebugLabelScope tonemapLabel{*this, frame.commandBuffer, tonemapPass.name, tonemapPass.debugColor};
        transitionImageTracked(frame.commandBuffer, swapchainImages_[imageIndex], swapchainStates_[imageIndex],
                               imageSyncStateFor(FrameGraphAccess::Write, FrameGraphUsage::ColorAttachment),
                               VK_IMAGE_ASPECT_COLOR_BIT);

        VkRenderingAttachmentInfo swapchainAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        swapchainAttachment.imageView = swapchainImageViews_[imageIndex];
        swapchainAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        swapchainAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        swapchainAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo tonemapRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        tonemapRenderInfo.renderArea.offset = {0, 0};
        tonemapRenderInfo.renderArea.extent = swapchainExtent_;
        tonemapRenderInfo.layerCount = 1;
        tonemapRenderInfo.colorAttachmentCount = 1;
        tonemapRenderInfo.pColorAttachments = &swapchainAttachment;
        vkCmdBeginRendering(frame.commandBuffer, &tonemapRenderInfo);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipelineLayout_, 0, 1, &tonemapDescriptorSet_, 0, nullptr);
        vkCmdPushConstants(frame.commandBuffer, tonemapPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(config_.exposure), &config_.exposure);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        renderImGui(frame.commandBuffer);
        vkCmdEndRendering(frame.commandBuffer);
    }

    if (screenshotReadback != nullptr) {
        const DebugLabelScope screenshotLabel{*this, frame.commandBuffer, screenshotPass.name, screenshotPass.debugColor};
        recordScreenshotCopy(frame.commandBuffer, imageIndex, *screenshotReadback);
    }

    transitionImageTracked(frame.commandBuffer, swapchainImages_[imageIndex], swapchainStates_[imageIndex],
                           finalImageSyncStateFor(frameGraph_.finalUsage(frameGraphResources_.swapchain)),
                           VK_IMAGE_ASPECT_COLOR_BIT);

    if (timestampsEnabled_) {
        vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestampQueryPool_,
                             static_cast<std::uint32_t>(frameIndex_) * kTimestampQueriesPerFrame + kTimestampFinalEnd);
    }

    checkVk(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer frame");
    const std::uint32_t scenePassCount = useDepthPrepass ? 2U : 1U;
    frame.submittedDepthPrepass = useDepthPrepass;
    frame.submittedScenePassCount = scenePassCount;
    stats_.depthPrepassEnabled = useDepthPrepass;
    stats_.scenePassCount = scenePassCount;
    stats_.sceneItemCount = static_cast<unsigned>(renderItems.size());
    stats_.visibleItemCount = visibility.visibleItemCount;
    stats_.sceneInstanceCapacity = static_cast<unsigned>(frame.instanceCapacity);
    stats_.sceneInstanceBufferMiB = bytesToMiB(frame.instanceData.size);
    stats_.meshBatchCount = sceneDrawCalls;
    const std::uint32_t sceneDrawCommandCount = (indirectSceneDrawsEnabled_ && sceneDrawCalls > 0U) ? 1U : sceneDrawCalls;
    stats_.drawCalls = (sceneDrawCommandCount * scenePassCount) + 1U;
    stats_.culledDrawCalls = visibility.culledDrawCalls;
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
    stats_.triangleCount = (visibility.sceneTriangleCount * scenePassCount) + 1ULL;
}

} // namespace ve
