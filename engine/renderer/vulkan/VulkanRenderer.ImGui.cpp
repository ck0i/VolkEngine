#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

void VulkanRenderer::Impl::createImGui() {
#if VOLKENGINE_ENABLE_IMGUI
    if (imguiOwner_.initialized) {
        return;
    }
    if (!config_.debugOverlay) {
        imguiOwner_.initialized = false;
        return;
    }

    bool contextCreated = false;
    bool glfwBackendInitialized = false;
    bool vulkanBackendInitAttempted = false;
    const auto cleanupPartialImGui = [&]() noexcept {
        if (vulkanBackendInitAttempted) {
            ImGui_ImplVulkan_Shutdown();
        }
        if (glfwBackendInitialized) {
            ImGui_ImplGlfw_Shutdown();
        }
        if (contextCreated) {
            ImGui::DestroyContext();
        }
    };

    try {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        contextCreated = true;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplGlfw_InitForVulkan(window_.handle(), false)) {
            throw std::runtime_error("Failed to initialize Dear ImGui GLFW backend");
        }
        glfwBackendInitialized = true;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = deviceOwner_.instance;
        initInfo.PhysicalDevice = deviceOwner_.physicalDevice;
        initInfo.Device = deviceOwner_.device;
        initInfo.QueueFamily = deviceOwner_.queueFamilies.graphics.value();
        initInfo.Queue = deviceOwner_.graphicsQueue;
        initInfo.DescriptorPoolSize = 32;
        initInfo.MinImageCount = swapchainOwner_.minimumImageCount;
        initInfo.ImageCount = static_cast<std::uint32_t>(swapchainOwner_.images.size());
        initInfo.PipelineCache = pipelineOwner_.cache;
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainOwner_.format;
        initInfo.CheckVkResultFn = [](VkResult result) { checkVk(result, "Dear ImGui Vulkan backend"); };
        initInfo.MinAllocationSize = 1024U * 1024U;

        vulkanBackendInitAttempted = true;
        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("Failed to initialize Dear ImGui Vulkan backend");
        }

        imguiOwner_.initialized = true;
        imguiOwner_.diagnosticsValid = false;
        imguiOwner_.diagnosticsRefreshSeconds = 0.0;
        imguiOwner_.minimumImageCount = initInfo.MinImageCount;
        imguiOwner_.imageCount = initInfo.ImageCount;
        imguiOwner_.swapchainFormat = swapchainOwner_.format;
        contextCreated = false;
        glfwBackendInitialized = false;
        vulkanBackendInitAttempted = false;
        logger()->info("Initialized Dear ImGui debug overlay");
    } catch (...) {
        cleanupPartialImGui();
        throw;
    }

#else
    imguiOwner_.initialized = false;
#endif
}

void VulkanRenderer::Impl::shutdownImGui() {
#if VOLKENGINE_ENABLE_IMGUI
    if (!imguiOwner_.initialized) {
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    imguiOwner_.initialized = false;
    imguiOwner_.minimumImageCount = 0;
    imguiOwner_.imageCount = 0;
    imguiOwner_.swapchainFormat = VK_FORMAT_UNDEFINED;
    imguiOwner_.diagnosticsValid = false;
    imguiOwner_.diagnosticsRefreshSeconds = 0.0;
#endif
}

void VulkanRenderer::Impl::beginImGuiFrame(const double frameDeltaMs) {
#if VOLKENGINE_ENABLE_IMGUI
    if (!config_.debugOverlay || !imguiOwner_.initialized) {
        return;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                       ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos({12.0f, 12.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    imguiOwner_.diagnosticsRefreshSeconds -= frameDeltaMs * 0.001;
    if (!imguiOwner_.diagnosticsValid || imguiOwner_.diagnosticsRefreshSeconds <= 0.0) {
        imguiOwner_.resourceStats = resourceOwner_.registry.stats();
        imguiOwner_.memoryUsageBytes = 0;
        imguiOwner_.memoryBudgetBytes = 0;
        if (deviceOwner_.memoryBudgetEnabled) {
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(deviceOwner_.allocator, budgets.data());
            for (std::uint32_t heapIndex = 0; heapIndex < deviceOwner_.physicalDeviceMemoryProperties.memoryHeapCount; ++heapIndex) {
                imguiOwner_.memoryUsageBytes += budgets[heapIndex].usage;
                imguiOwner_.memoryBudgetBytes += budgets[heapIndex].budget;
            }
        }
        imguiOwner_.diagnosticsRefreshSeconds = kImGuiDiagnosticsRefreshIntervalSeconds;
        imguiOwner_.diagnosticsValid = true;
    }
    const GpuResourceRegistry::Stats& resourceStats = imguiOwner_.resourceStats;
    if (ImGui::Begin("VolkEngine renderer stats", nullptr, flags)) {
        ImGui::TextUnformatted("VolkEngine Vulkan renderer");
        ImGui::Text("GPU: %s (%s)", deviceOwner_.info.adapterName.c_str(), gpuClassName(deviceOwner_.info.discreteGpu));
        ImGui::Text("Vulkan: %u.%u.%u  max2D: %u",
                    deviceOwner_.info.apiVersionMajor, deviceOwner_.info.apiVersionMinor,
                    deviceOwner_.info.apiVersionPatch, deviceOwner_.info.maxImageDimension2D);
        ImGui::Separator();
        ImGui::Text("Frame delta: %.3f ms", frameDeltaMs);
        ImGui::Text("CPU render-submit: %.3f ms", stats_.cpuFrameMs);
        ImGui::Text("CPU phases: scene %.3f / prepare %.3f / record %.3f / submit %.3f ms",
                    stats_.cpuSceneBuildMs, stats_.cpuPrepareMs, stats_.cpuCommandRecordMs, stats_.cpuQueueSubmitMs);
        if (stats_.gpuTimestampsValid) {
            ImGui::Text("GPU frame: %.3f ms (depth %.3f / HDR %.3f / final %.3f)",
                        stats_.gpuFrameMs, stats_.gpuDepthPrepassMs, stats_.gpuHdrSceneMs, stats_.gpuFinalPassMs);
        } else {
            ImGui::TextUnformatted("GPU frame: pending/unavailable");
        }
        ImGui::Text("Draws: %u  Culled items: %u  Grid tiles: %u/%u accepted, %u culled, %u intersected",
                    stats_.drawCalls, stats_.culledItemCount, stats_.gridTilesAccepted, stats_.gridTileCount,
                    stats_.gridTilesCulled, stats_.gridTilesIntersected);
        ImGui::Text("Grid visibility cache: %s  Work records: %u",
                    stats_.gridVisibilityCacheHit ? "hit" : "miss", stats_.gridVisibilityWorkItems);
        ImGui::Text("Triangles: %llu scene / %llu submitted", static_cast<unsigned long long>(stats_.sceneTriangleCount), static_cast<unsigned long long>(stats_.triangleCount));
        ImGui::Text("Sphere LOD instances: %u high / %u medium / %u low",
                    stats_.sphereLodHighCount, stats_.sphereLodMediumCount, stats_.sphereLodLowCount);
        ImGui::Text("Exposure: %.2f  VSync: %s  Depth prepass: %s (%s)", config_.exposure, config_.vsync ? "on" : "off", stats_.depthPrepassEnabled ? "on" : "off", depthPrepassModeName(config_.depthPrepassMode));
        ImGui::Text("Scene: %u items, %u visible, %u mesh batches, %u scene passes, %s",
                    stats_.sceneItemCount, stats_.visibleItemCount, stats_.meshBatchCount, stats_.scenePassCount,
                    stats_.indirectSceneDraws ? "multi-draw indirect" : "direct batched");
        ImGui::Text("Upload sync: %s  max indirect draws: %u",
                    transferUploadSyncName(deviceOwner_.info.transferUploadSync), deviceOwner_.info.maxDrawIndirectCount);
        ImGui::Text("Instance storage: %u capacity (%.2f MiB)",
                    stats_.sceneInstanceCapacity, stats_.sceneInstanceBufferMiB);
        ImGui::Text("Swapchain: %ux%u  images: %u/%u  present: %s",
                    swapchainOwner_.extent.width, swapchainOwner_.extent.height, imguiOwner_.imageCount, imguiOwner_.minimumImageCount, presentModeName(swapchainOwner_.presentMode).data());
        ImGui::Text("Validation: %s", deviceOwner_.validationEnabled ? "enabled" : (config_.validation ? "requested unavailable" : "off"));
        if (deviceOwner_.memoryBudgetEnabled) {
            ImGui::Text("VMA memory budget: %.1f / %.1f MiB",
                        bytesToMiB(imguiOwner_.memoryUsageBytes),
                        bytesToMiB(imguiOwner_.memoryBudgetBytes));
        } else {
            ImGui::TextUnformatted("VMA memory budget: unavailable");
        }
        ImGui::Text("Descriptor indexing: %s  bindless sampled-image support: %s",
                    deviceOwner_.info.descriptorIndexing ? "supported" : "unavailable",
                    deviceOwner_.info.bindlessSampledImagesSupported ? "supported" : "unavailable");
        ImGui::Text("Texture sampling: anisotropy %s (%.1fx)",
                    deviceOwner_.info.samplerAnisotropy ? "enabled" : "off",
                    deviceOwner_.info.maxSamplerAnisotropy);
        ImGui::Text("Frame graph: %u passes, %u logical resources, %u barriers, %u physical allocations",
                    stats_.graphPassCount, stats_.graphResourceCount, stats_.graphBarrierCount,
                    stats_.graphPhysicalAllocationCount);
        ImGui::Text("Frame graph memory: %.2f / %.2f MiB requested/allocated; compile %.3f ms, generation %llu (%s)",
                    bytesToMiB(stats_.graphTransientRequestedBytes),
                    bytesToMiB(stats_.graphTransientAllocatedBytes), stats_.cpuGraphCompileMs,
                    static_cast<unsigned long long>(stats_.graphRecompileCount),
                    stats_.graphLastCompileWasResize ? "resize" : "startup");
        ImGui::Text("GPU resources: %u live (%u buffers, %u images, %u imported), %.2f MiB",
                    resourceStats.liveResources, resourceStats.buffers, resourceStats.images,
                    resourceStats.importedImages, bytesToMiB(resourceStats.bytes));
        ImGui::Text("Resource bytes: buffers %.2f MiB, owned images %.2f MiB, imported images %.2f MiB",
                    bytesToMiB(resourceStats.bufferBytes), bytesToMiB(resourceStats.ownedImageBytes),
                    bytesToMiB(resourceStats.importedImageBytes));
    }
    ImGui::End();
    ImGui::Render();
#else
    (void)frameDeltaMs;
#endif
}

void VulkanRenderer::Impl::renderImGui(const VkCommandBuffer commandBuffer) const {
#if VOLKENGINE_ENABLE_IMGUI
    if (!config_.debugOverlay || !imguiOwner_.initialized) {
        return;
    }
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }
    ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
#else
    (void)commandBuffer;
#endif
}

} // namespace ve
