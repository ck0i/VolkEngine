#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

std::filesystem::path VulkanRenderer::Impl::pipelineCachePath() const {
    return config_.cacheDirectory / "pipeline_cache.bin";
}

bool VulkanRenderer::Impl::pipelineCacheDataMatchesDevice(const std::vector<std::byte>& data) const {
    if (data.size() < sizeof(PipelineCacheHeader)) {
        return false;
    }

    PipelineCacheHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.headerSize < sizeof(PipelineCacheHeader) || static_cast<std::size_t>(header.headerSize) > data.size()) {
        return false;
    }
    if (header.headerVersion != static_cast<std::uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) {
        return false;
    }
    if (header.vendorID != physicalDeviceProperties_.vendorID || header.deviceID != physicalDeviceProperties_.deviceID) {
        return false;
    }
    return std::equal(header.pipelineCacheUUID.begin(), header.pipelineCacheUUID.end(), physicalDeviceProperties_.pipelineCacheUUID);
}

std::vector<std::byte> VulkanRenderer::Impl::loadPipelineCacheData() const {
    const std::filesystem::path path = pipelineCachePath();
    if (!std::filesystem::exists(path)) {
        return {};
    }

    try {
        std::vector<std::byte> data = readBinaryFile(path);
        if (pipelineCacheDataMatchesDevice(data)) {
            logger()->info("Loaded Vulkan pipeline cache: {} bytes from {}", data.size(), path.string());
            return data;
        }
        logger()->warn("Ignoring incompatible Vulkan pipeline cache at {}", path.string());
    } catch (const std::exception& e) {
        logger()->warn("Failed to load Vulkan pipeline cache {}: {}", path.string(), e.what());
    }
    return {};
}

void VulkanRenderer::Impl::createPipelineCache() {
    const std::vector<std::byte> initialData = loadPipelineCacheData();
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = initialData.size();
    cacheInfo.pInitialData = initialData.empty() ? nullptr : initialData.data();
    checkVk(vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_), "vkCreatePipelineCache");
    setObjectName(VK_OBJECT_TYPE_PIPELINE_CACHE, handleToUint64(pipelineCache_), "Renderer Pipeline Cache");
}

void VulkanRenderer::Impl::savePipelineCache() const {
    std::size_t dataSize = 0;
    VkResult result = vkGetPipelineCacheData(device_, pipelineCache_, &dataSize, nullptr);
    if (result != VK_SUCCESS || dataSize == 0U) {
        logger()->warn("Skipping Vulkan pipeline cache save; size query returned {}", static_cast<int>(result));
        return;
    }

    std::vector<std::byte> data;
    for (std::uint32_t attempt = 0; attempt < 3U; ++attempt) {
        data.assign(dataSize, std::byte{});
        std::size_t writableSize = data.size();
        result = vkGetPipelineCacheData(device_, pipelineCache_, &writableSize, data.data());
        if (result == VK_SUCCESS) {
            data.resize(writableSize);
            break;
        }
        if (result != VK_INCOMPLETE) {
            logger()->warn("Skipping Vulkan pipeline cache save; read returned {}", static_cast<int>(result));
            return;
        }

        result = vkGetPipelineCacheData(device_, pipelineCache_, &dataSize, nullptr);
        if (result != VK_SUCCESS || dataSize == 0U) {
            logger()->warn("Skipping Vulkan pipeline cache save; retry size query returned {}", static_cast<int>(result));
            return;
        }
    }
    if (result != VK_SUCCESS) {
        logger()->warn("Skipping Vulkan pipeline cache save; cache kept growing during readback");
        return;
    }

    if (!pipelineCacheDataMatchesDevice(data)) {
        logger()->warn("Skipping Vulkan pipeline cache save; generated cache header does not match selected device");
        return;
    }

    const std::filesystem::path path = pipelineCachePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        logger()->warn("Failed to create pipeline cache directory {}: {}", path.parent_path().string(), error.message());
        return;
    }

    const std::filesystem::path temporaryPath = uniquePipelineCacheTemporaryPath(path);
    {
        std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!file) {
            logger()->warn("Failed to open temporary Vulkan pipeline cache for writing: {}", temporaryPath.string());
            return;
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        file.close();
        if (!file) {
            logger()->warn("Failed to write temporary Vulkan pipeline cache: {}", temporaryPath.string());
            std::filesystem::remove(temporaryPath, error);
            return;
        }
    }

    error.clear();
    std::filesystem::rename(temporaryPath, path, error);
    if (error) {
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        error.clear();
        std::filesystem::rename(temporaryPath, path, error);
    }
    if (error) {
        logger()->warn("Failed to publish Vulkan pipeline cache {}; discarded temporary cache {}: {}",
                       path.string(), temporaryPath.string(), error.message());
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return;
    }
    logger()->info("Saved Vulkan pipeline cache: {} bytes to {}", data.size(), path.string());
}

VulkanRenderer::Impl::PipelineSet VulkanRenderer::Impl::buildPipelineSet() {
    PipelineSet pipelines{};
    const std::array<std::filesystem::path, 4> shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    VkShaderModule sceneVert = VK_NULL_HANDLE;
    VkShaderModule sceneFrag = VK_NULL_HANDLE;
    VkShaderModule tonemapVert = VK_NULL_HANDLE;
    VkShaderModule tonemapFrag = VK_NULL_HANDLE;

    try {
        sceneVert = createShaderModule(shaderPaths[0]);
        sceneFrag = createShaderModule(shaderPaths[1]);
        tonemapVert = createShaderModule(shaderPaths[2]);
        tonemapFrag = createShaderModule(shaderPaths[3]);

        std::array<VkPipelineShaderStageCreateInfo, 2> sceneStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, sceneVert), shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, sceneFrag)};
        std::array<VkVertexInputBindingDescription, 1> bindings{};
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(Vertex);
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 3> attributes{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)};

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthPrepassDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthPrepassDepth.depthTestEnable = VK_TRUE;
        depthPrepassDepth.depthWriteEnable = VK_TRUE;
        depthPrepassDepth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineDepthStencilStateCreateInfo sceneDepth = depthPrepassDepth;
        sceneDepth.depthWriteEnable = VK_FALSE;
        sceneDepth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkPipelineColorBlendStateCreateInfo noColorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        noColorBlend.attachmentCount = 0;

        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        VkPipelineLayoutCreateInfo sceneLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        sceneLayoutInfo.setLayoutCount = 1;
        sceneLayoutInfo.pSetLayouts = &sceneSetLayout_;
        checkVk(vkCreatePipelineLayout(device_, &sceneLayoutInfo, nullptr, &pipelines.sceneLayout), "vkCreatePipelineLayout scene");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, handleToUint64(pipelines.sceneLayout), "Scene Pipeline Layout");

        VkPipelineRenderingCreateInfo sceneRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        sceneRendering.colorAttachmentCount = 1;
        sceneRendering.pColorAttachmentFormats = &hdr_.format;
        sceneRendering.depthAttachmentFormat = depth_.format;

        std::array<VkPipelineShaderStageCreateInfo, 1> depthPrepassStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, sceneVert)};
        VkPipelineRenderingCreateInfo depthPrepassRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        depthPrepassRendering.colorAttachmentCount = 0;
        depthPrepassRendering.depthAttachmentFormat = depth_.format;
        VkGraphicsPipelineCreateInfo depthPrepassInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        depthPrepassInfo.pNext = &depthPrepassRendering;
        depthPrepassInfo.stageCount = static_cast<std::uint32_t>(depthPrepassStages.size());
        depthPrepassInfo.pStages = depthPrepassStages.data();
        depthPrepassInfo.pVertexInputState = &vertexInput;
        depthPrepassInfo.pInputAssemblyState = &inputAssembly;
        depthPrepassInfo.pViewportState = &viewportState;
        depthPrepassInfo.pRasterizationState = &rasterizer;
        depthPrepassInfo.pMultisampleState = &multisample;
        depthPrepassInfo.pDepthStencilState = &depthPrepassDepth;
        depthPrepassInfo.pColorBlendState = &noColorBlend;
        depthPrepassInfo.pDynamicState = &dynamic;
        depthPrepassInfo.layout = pipelines.sceneLayout;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &depthPrepassInfo, nullptr, &pipelines.depthPrepass), "vkCreateGraphicsPipelines depth prepass");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.depthPrepass), "Depth Prepass Pipeline");

        VkGraphicsPipelineCreateInfo sceneInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        sceneInfo.pNext = &sceneRendering;
        sceneInfo.stageCount = static_cast<std::uint32_t>(sceneStages.size());
        sceneInfo.pStages = sceneStages.data();
        sceneInfo.pVertexInputState = &vertexInput;
        sceneInfo.pInputAssemblyState = &inputAssembly;
        sceneInfo.pViewportState = &viewportState;
        sceneInfo.pRasterizationState = &rasterizer;
        sceneInfo.pMultisampleState = &multisample;
        sceneInfo.pDepthStencilState = &sceneDepth;
        sceneInfo.pColorBlendState = &blend;
        sceneInfo.pDynamicState = &dynamic;
        sceneInfo.layout = pipelines.sceneLayout;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &sceneInfo, nullptr, &pipelines.scene), "vkCreateGraphicsPipelines scene");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.scene), "HDR Scene Pipeline");

        sceneInfo.pDepthStencilState = &depthPrepassDepth;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &sceneInfo, nullptr, &pipelines.sceneNoPrepass), "vkCreateGraphicsPipelines scene no prepass");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.sceneNoPrepass), "HDR Scene Pipeline No Prepass");

        std::array<VkPipelineShaderStageCreateInfo, 2> tonemapStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, tonemapVert), shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, tonemapFrag)};
        VkPipelineVertexInputStateCreateInfo emptyVertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo noDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo noCullRasterizer = rasterizer;
        noCullRasterizer.cullMode = VK_CULL_MODE_NONE;

        VkPushConstantRange tonemapPushRange{};
        tonemapPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        tonemapPushRange.offset = 0;
        tonemapPushRange.size = sizeof(float);

        VkPipelineLayoutCreateInfo tonemapLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        tonemapLayoutInfo.setLayoutCount = 1;
        tonemapLayoutInfo.pSetLayouts = &tonemapSetLayout_;
        tonemapLayoutInfo.pushConstantRangeCount = 1;
        tonemapLayoutInfo.pPushConstantRanges = &tonemapPushRange;
        checkVk(vkCreatePipelineLayout(device_, &tonemapLayoutInfo, nullptr, &pipelines.tonemapLayout), "vkCreatePipelineLayout tonemap");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, handleToUint64(pipelines.tonemapLayout), "Tonemap Pipeline Layout");

        VkPipelineRenderingCreateInfo tonemapRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        tonemapRendering.colorAttachmentCount = 1;
        tonemapRendering.pColorAttachmentFormats = &swapchainFormat_;

        VkGraphicsPipelineCreateInfo tonemapInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        tonemapInfo.pNext = &tonemapRendering;
        tonemapInfo.stageCount = static_cast<std::uint32_t>(tonemapStages.size());
        tonemapInfo.pStages = tonemapStages.data();
        tonemapInfo.pVertexInputState = &emptyVertexInput;
        tonemapInfo.pInputAssemblyState = &inputAssembly;
        tonemapInfo.pViewportState = &viewportState;
        tonemapInfo.pRasterizationState = &noCullRasterizer;
        tonemapInfo.pMultisampleState = &multisample;
        tonemapInfo.pDepthStencilState = &noDepth;
        tonemapInfo.pColorBlendState = &blend;
        tonemapInfo.pDynamicState = &dynamic;
        tonemapInfo.layout = pipelines.tonemapLayout;
        checkVk(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &tonemapInfo, nullptr, &pipelines.tonemap), "vkCreateGraphicsPipelines tonemap");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.tonemap), "Tonemap Pipeline");
    } catch (...) {
        if (tonemapFrag != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, tonemapFrag, nullptr); }
        if (tonemapVert != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, tonemapVert, nullptr); }
        if (sceneFrag != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, sceneFrag, nullptr); }
        if (sceneVert != VK_NULL_HANDLE) { vkDestroyShaderModule(device_, sceneVert, nullptr); }
        destroyPipelineSet(pipelines);
        throw;
    }

    vkDestroyShaderModule(device_, tonemapFrag, nullptr);
    vkDestroyShaderModule(device_, tonemapVert, nullptr);
    vkDestroyShaderModule(device_, sceneFrag, nullptr);
    vkDestroyShaderModule(device_, sceneVert, nullptr);
    return pipelines;
}

void VulkanRenderer::Impl::createPipelines() {
    PipelineSet pipelines = buildPipelineSet();
    installPipelineSet(pipelines);
    refreshShaderWriteTimes();
}

void VulkanRenderer::Impl::destroyPipelineSet(PipelineSet& pipelines) const {
    if (pipelines.tonemap != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.tonemap, nullptr); pipelines.tonemap = VK_NULL_HANDLE; }
    if (pipelines.tonemapLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, pipelines.tonemapLayout, nullptr); pipelines.tonemapLayout = VK_NULL_HANDLE; }
    if (pipelines.depthPrepass != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.depthPrepass, nullptr); pipelines.depthPrepass = VK_NULL_HANDLE; }
    if (pipelines.scene != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.scene, nullptr); pipelines.scene = VK_NULL_HANDLE; }
    if (pipelines.sceneNoPrepass != VK_NULL_HANDLE) { vkDestroyPipeline(device_, pipelines.sceneNoPrepass, nullptr); pipelines.sceneNoPrepass = VK_NULL_HANDLE; }
    if (pipelines.sceneLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device_, pipelines.sceneLayout, nullptr); pipelines.sceneLayout = VK_NULL_HANDLE; }
}

void VulkanRenderer::Impl::retireDeferredPipelineSets() {
    for (auto it = retiredPipelineSets_.begin(); it != retiredPipelineSets_.end();) {
        bool ready = true;
        for (const VkFence fence : it->completionFences) {
            if (fence == VK_NULL_HANDLE) {
                continue;
            }
            const VkResult status = vkGetFenceStatus(device_, fence);
            if (status == VK_NOT_READY) {
                ready = false;
                break;
            }
            checkVk(status, "vkGetFenceStatus retired pipeline set");
        }
        if (ready) {
            destroyPipelineSet(it->pipelines);
            it = retiredPipelineSets_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanRenderer::Impl::installPipelineSet(const PipelineSet& pipelines) {
    scenePipelineLayout_ = pipelines.sceneLayout;
    depthPrepassPipeline_ = pipelines.depthPrepass;
    scenePipeline_ = pipelines.scene;
    sceneNoPrepassPipeline_ = pipelines.sceneNoPrepass;
    tonemapPipelineLayout_ = pipelines.tonemapLayout;
    tonemapPipeline_ = pipelines.tonemap;
}

void VulkanRenderer::Impl::refreshShaderWriteTimes() {
    const std::array<std::filesystem::path, 4> shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    for (std::size_t i = 0; i < shaderPaths.size(); ++i) {
        std::error_code error;
        shaderWriteTimes_[i] = std::filesystem::last_write_time(shaderPaths[i], error);
        if (error) {
            shaderWriteTimes_[i] = {};
        }
    }
}

bool VulkanRenderer::Impl::shaderFilesChanged() const {
    const std::array<std::filesystem::path, 4> shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    for (std::size_t i = 0; i < shaderPaths.size(); ++i) {
        std::error_code error;
        const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(shaderPaths[i], error);
        if (error) {
            return false;
        }
        if (writeTime != shaderWriteTimes_[i]) {
            return true;
        }
    }
    return false;
}

void VulkanRenderer::Impl::pollShaderHotReload(const double elapsedSeconds) {
    if (!config_.shaderHotReload || elapsedSeconds - shaderHotReloadLastCheckSeconds_ < 0.5) {
        return;
    }
    shaderHotReloadLastCheckSeconds_ = elapsedSeconds;
    if (!shaderFilesChanged()) {
        return;
    }

    logger()->info("Detected shader bytecode change; rebuilding graphics pipelines");
    PipelineSet nextPipelines{};
    try {
        nextPipelines = buildPipelineSet();
    } catch (const std::exception& e) {
        logger()->warn("Shader hot reload failed; keeping existing pipelines: {}", e.what());
        refreshShaderWriteTimes();
        return;
    }

    PipelineSet oldPipelines{};
    oldPipelines.sceneLayout = scenePipelineLayout_;
    oldPipelines.depthPrepass = depthPrepassPipeline_;
    oldPipelines.scene = scenePipeline_;
    oldPipelines.sceneNoPrepass = sceneNoPrepassPipeline_;
    oldPipelines.tonemapLayout = tonemapPipelineLayout_;
    oldPipelines.tonemap = tonemapPipeline_;

    RetiredPipelineSet retired{};
    retired.pipelines = oldPipelines;
    for (std::size_t index = 0; index < frames_.size(); ++index) {
        retired.completionFences[index] = frames_[index].submittedOnce ? frames_[index].inFlight : VK_NULL_HANDLE;
    }
    try {
        retiredPipelineSets_.push_back(retired);
    } catch (...) {
        destroyPipelineSet(nextPipelines);
        throw;
    }
    installPipelineSet(nextPipelines);
    refreshShaderWriteTimes();
    logger()->info("Reloaded graphics pipelines from updated shader bytecode; retiring previous set after tracked frame fences signal");
}

VkShaderModule VulkanRenderer::Impl::createShaderModule(const std::filesystem::path& path) const {
    const std::vector<std::byte> bytes = readBinaryFile(path);
    constexpr std::uint32_t kSpirvMagic = 0x07230203U;
    constexpr std::uint32_t kByteSwappedSpirvMagic = 0x03022307U;
    constexpr std::size_t kSpirvHeaderWordCount = 5;
    if (bytes.size() < kSpirvHeaderWordCount * sizeof(std::uint32_t)) {
        throw std::runtime_error("SPIR-V file is too small to contain a valid header: " + path.string());
    }
    if (bytes.size() % sizeof(std::uint32_t) != 0U) {
        throw std::runtime_error("SPIR-V file has invalid byte size: " + path.string());
    }

    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    if (words[0] == kByteSwappedSpirvMagic) {
        throw std::runtime_error("SPIR-V file appears byte-swapped instead of native little-endian words: " + path.string());
    }
    if (words[0] != kSpirvMagic) {
        throw std::runtime_error("SPIR-V file has invalid magic number: " + path.string());
    }

    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = bytes.size();
    createInfo.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const std::string operation = "vkCreateShaderModule " + path.string();
    checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &module), operation.c_str());
    return module;
}

} // namespace ve
