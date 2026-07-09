#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

void VulkanRenderer::Impl::createImGui() {
#if VOLKENGINE_ENABLE_IMGUI
    if (imguiInitialized_) {
        return;
    }
    if (!config_.debugOverlay) {
        imguiInitialized_ = false;
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
        initInfo.Instance = instance_;
        initInfo.PhysicalDevice = physicalDevice_;
        initInfo.Device = device_;
        initInfo.QueueFamily = queueFamilies_.graphics.value();
        initInfo.Queue = graphicsQueue_;
        initInfo.DescriptorPoolSize = 32;
        initInfo.MinImageCount = swapchainMinImageCount_;
        initInfo.ImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
        initInfo.PipelineCache = pipelineCache_;
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat_;
        initInfo.CheckVkResultFn = [](VkResult result) { checkVk(result, "Dear ImGui Vulkan backend"); };
        initInfo.MinAllocationSize = 1024U * 1024U;

        vulkanBackendInitAttempted = true;
        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("Failed to initialize Dear ImGui Vulkan backend");
        }

        imguiInitialized_ = true;
        imguiDiagnosticsValid_ = false;
        imguiDiagnosticsRefreshSeconds_ = 0.0;
        imguiMinImageCount_ = initInfo.MinImageCount;
        imguiImageCount_ = initInfo.ImageCount;
        imguiSwapchainFormat_ = swapchainFormat_;
        contextCreated = false;
        glfwBackendInitialized = false;
        vulkanBackendInitAttempted = false;
        logger()->info("Initialized Dear ImGui debug overlay");
    } catch (...) {
        cleanupPartialImGui();
        throw;
    }

#else
    imguiInitialized_ = false;
#endif
}

void VulkanRenderer::Impl::shutdownImGui() {
#if VOLKENGINE_ENABLE_IMGUI
    if (!imguiInitialized_) {
        return;
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized_ = false;
    imguiMinImageCount_ = 0;
    imguiImageCount_ = 0;
    imguiSwapchainFormat_ = VK_FORMAT_UNDEFINED;
    imguiDiagnosticsValid_ = false;
    imguiDiagnosticsRefreshSeconds_ = 0.0;
#endif
}

void VulkanRenderer::Impl::beginImGuiFrame(const double frameDeltaMs) {
#if VOLKENGINE_ENABLE_IMGUI
    if (!config_.debugOverlay || !imguiInitialized_) {
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
    imguiDiagnosticsRefreshSeconds_ -= frameDeltaMs * 0.001;
    if (!imguiDiagnosticsValid_ || imguiDiagnosticsRefreshSeconds_ <= 0.0) {
        imguiResourceStats_ = resourceRegistry_.stats();
        imguiMemoryUsageBytes_ = 0;
        imguiMemoryBudgetBytes_ = 0;
        if (memoryBudgetEnabled_) {
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
            vmaGetHeapBudgets(allocator_, budgets.data());
            for (std::uint32_t heapIndex = 0; heapIndex < physicalDeviceMemoryProperties_.memoryHeapCount; ++heapIndex) {
                imguiMemoryUsageBytes_ += budgets[heapIndex].usage;
                imguiMemoryBudgetBytes_ += budgets[heapIndex].budget;
            }
        }
        imguiDiagnosticsRefreshSeconds_ = kImGuiDiagnosticsRefreshIntervalSeconds;
        imguiDiagnosticsValid_ = true;
    }
    const GpuResourceRegistry::Stats& resourceStats = imguiResourceStats_;
    if (ImGui::Begin("VolkEngine renderer stats", nullptr, flags)) {
        ImGui::TextUnformatted("VolkEngine Vulkan renderer");
        ImGui::Text("GPU: %s (%s)", deviceInfo_.adapterName.c_str(), gpuClassName(deviceInfo_.discreteGpu));
        ImGui::Text("Vulkan: %u.%u.%u  max2D: %u",
                    deviceInfo_.apiVersionMajor, deviceInfo_.apiVersionMinor,
                    deviceInfo_.apiVersionPatch, deviceInfo_.maxImageDimension2D);
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
        ImGui::Text("Draws: %u  Culled: %u  Grid tiles: %u/%u accepted, %u culled, %u intersected",
                    stats_.drawCalls, stats_.culledDrawCalls, stats_.gridTilesAccepted, stats_.gridTileCount,
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
                    transferUploadSyncName(deviceInfo_.transferUploadSync), deviceInfo_.maxDrawIndirectCount);
        ImGui::Text("Instance storage: %u capacity (%.2f MiB)",
                    stats_.sceneInstanceCapacity, stats_.sceneInstanceBufferMiB);
        ImGui::Text("Swapchain: %ux%u  images: %u/%u  present: %s",
                    swapchainExtent_.width, swapchainExtent_.height, imguiImageCount_, imguiMinImageCount_, presentModeName(presentMode_).data());
        ImGui::Text("Validation: %s", validationEnabled_ ? "enabled" : (config_.validation ? "requested unavailable" : "off"));
        if (memoryBudgetEnabled_) {
            ImGui::Text("VMA memory budget: %.1f / %.1f MiB",
                        bytesToMiB(imguiMemoryUsageBytes_),
                        bytesToMiB(imguiMemoryBudgetBytes_));
        } else {
            ImGui::TextUnformatted("VMA memory budget: unavailable");
        }
        ImGui::Text("Descriptor indexing: %s  bindless sampled-image support: %s",
                    deviceInfo_.descriptorIndexing ? "supported" : "unavailable",
                    deviceInfo_.bindlessSampledImagesSupported ? "supported" : "unavailable");
        ImGui::Text("Texture sampling: anisotropy %s (%.1fx)",
                    deviceInfo_.samplerAnisotropy ? "enabled" : "off",
                    deviceInfo_.maxSamplerAnisotropy);
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
    if (!config_.debugOverlay || !imguiInitialized_) {
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
