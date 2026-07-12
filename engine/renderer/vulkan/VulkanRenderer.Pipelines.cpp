#include "renderer/vulkan/VulkanRendererImpl.hpp"

#include <chrono>
#include <fstream>
#include <random>

namespace ve {
namespace {
constexpr double kShaderHotReloadInitialRetryDelaySeconds = 0.5;
constexpr double kShaderHotReloadMaxRetryDelaySeconds = 4.0;

[[nodiscard]] std::vector<std::uint32_t> readSpirvWords(const std::filesystem::path& path) {
    constexpr std::uint32_t kSpirvMagic = 0x07230203U;
    constexpr std::uint32_t kByteSwappedSpirvMagic = 0x03022307U;
    constexpr std::size_t kSpirvHeaderWordCount = 5;

    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open SPIR-V file: " + path.string());
    }
    const std::streamsize byteSize = file.tellg();
    if (byteSize < 0) {
        throw std::runtime_error("Failed to determine SPIR-V file size: " + path.string());
    }
    if (static_cast<std::uint64_t>(byteSize) < kSpirvHeaderWordCount * sizeof(std::uint32_t)) {
        throw std::runtime_error("SPIR-V file is too small to contain a valid header: " + path.string());
    }
    if ((byteSize % static_cast<std::streamsize>(sizeof(std::uint32_t))) != 0) {
        throw std::runtime_error("SPIR-V file has invalid byte size: " + path.string());
    }

    std::vector<std::uint32_t> words(static_cast<std::size_t>(byteSize) / sizeof(std::uint32_t));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(words.data()), byteSize)) {
        throw std::runtime_error("Failed to read SPIR-V file: " + path.string());
    }
    if (words[0] == kByteSwappedSpirvMagic) {
        throw std::runtime_error("SPIR-V file appears byte-swapped instead of "
                             "native little-endian words: " + path.string());
    }
    if (words[0] != kSpirvMagic) {
        throw std::runtime_error("SPIR-V file has invalid magic number: " + path.string());
    }
    return words;
}

[[nodiscard]] std::filesystem::path uniquePipelineCacheTemporaryPath(const std::filesystem::path& path) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device random;
    const std::uint64_t salt = (static_cast<std::uint64_t>(random()) << 32U) ^ static_cast<std::uint64_t>(random());

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp.";
    temporaryPath += std::to_string(ticks);
    temporaryPath += ".";
    temporaryPath += std::to_string(salt);
    return temporaryPath;
}
} // namespace


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
    if (header.vendorID != deviceOwner_.physicalDeviceProperties.vendorID || header.deviceID != deviceOwner_.physicalDeviceProperties.deviceID) {
        return false;
    }
    return std::equal(header.pipelineCacheUUID.begin(), header.pipelineCacheUUID.end(), deviceOwner_.physicalDeviceProperties.pipelineCacheUUID);
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
    checkVk(vkCreatePipelineCache(deviceOwner_.device, &cacheInfo, nullptr, &pipelineOwner_.cache), "vkCreatePipelineCache");
    setObjectName(VK_OBJECT_TYPE_PIPELINE_CACHE, handleToUint64(pipelineOwner_.cache), "Renderer Pipeline Cache");
}

void VulkanRenderer::Impl::savePipelineCache() const {
    std::size_t dataSize = 0;
    VkResult result = vkGetPipelineCacheData(deviceOwner_.device, pipelineOwner_.cache, &dataSize, nullptr);
    if (result != VK_SUCCESS || dataSize == 0U) {
        logger()->warn("Skipping Vulkan pipeline cache save; size query returned {}", static_cast<int>(result));
        return;
    }

    std::vector<std::byte> data;
    for (std::uint32_t attempt = 0; attempt < 3U; ++attempt) {
        data.assign(dataSize, std::byte{});
        std::size_t writableSize = data.size();
        result = vkGetPipelineCacheData(deviceOwner_.device, pipelineOwner_.cache, &writableSize, data.data());
        if (result == VK_SUCCESS) {
            data.resize(writableSize);
            break;
        }
        if (result != VK_INCOMPLETE) {
            logger()->warn("Skipping Vulkan pipeline cache save; read returned {}", static_cast<int>(result));
            return;
        }

        result = vkGetPipelineCacheData(deviceOwner_.device, pipelineOwner_.cache, &dataSize, nullptr);
        if (result != VK_SUCCESS || dataSize == 0U) {
            logger()->warn("Skipping Vulkan pipeline cache save; retry size query returned {}", static_cast<int>(result));
            return;
        }
    }
    if (result != VK_SUCCESS) {
        logger()->warn("Skipping Vulkan pipeline cache save; cache kept growing "
                   "during readback");
        return;
    }

    if (!pipelineCacheDataMatchesDevice(data)) {
        logger()->warn("Skipping Vulkan pipeline cache save; generated cache "
                   "header does not match selected device");
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
        logger()->warn("Failed to publish Vulkan pipeline cache {}; discarded "
                   "temporary cache {}: {}",
                       path.string(), temporaryPath.string(), error.message());
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return;
    }
    logger()->info("Saved Vulkan pipeline cache: {} bytes to {}", data.size(), path.string());
}

VulkanRenderer::Impl::PipelineSet VulkanRenderer::Impl::buildPipelineSet() {
    PipelineSet pipelines{};
    const auto shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    VkShaderModule sceneVert = VK_NULL_HANDLE;
    VkShaderModule sceneFrag = VK_NULL_HANDLE;
    VkShaderModule tonemapVert = VK_NULL_HANDLE;
    VkShaderModule tonemapFrag = VK_NULL_HANDLE;
    VkShaderModule atmosphereFrag = VK_NULL_HANDLE;
  VkShaderModule depthPrepassVert = VK_NULL_HANDLE;
    VkShaderModule depthPrepassOpaqueVert = VK_NULL_HANDLE;
    VkShaderModule cullCompute = VK_NULL_HANDLE;
    VkShaderModule cullSubgroupCompute = VK_NULL_HANDLE;
    VkShaderModule depthPyramidCompute = VK_NULL_HANDLE;
    VkShaderModule lightAssignmentCompute = VK_NULL_HANDLE;
    VkShaderModule shadowVert = VK_NULL_HANDLE;
    VkShaderModule shadowOpaqueVert = VK_NULL_HANDLE;
    VkShaderModule shadowFrag = VK_NULL_HANDLE;

    try {
        sceneVert = createShaderModule(
            shaderPaths[indirectSceneDrawsEnabled_ ? 7U : 0U]);
        sceneFrag = createShaderModule(
            shaderPaths[resourceOwner_.bindlessMaterialsEnabled ? 5U : 1U]);
        tonemapVert = createShaderModule(shaderPaths[2]);
        tonemapFrag = createShaderModule(shaderPaths[3]);
    atmosphereFrag = createShaderModule(shaderPaths[18]);
        depthPrepassVert = createShaderModule(
            shaderPaths[indirectSceneDrawsEnabled_ ? 8U : 4U]);
        depthPrepassOpaqueVert = createShaderModule(
            shaderPaths[indirectSceneDrawsEnabled_ ? 17U : 16U]);
        cullCompute = createShaderModule(shaderPaths[6]);
        depthPyramidCompute = createShaderModule(shaderPaths[9]);
        lightAssignmentCompute = createShaderModule(shaderPaths[11]);
        shadowVert = createShaderModule(shaderPaths[12]);
        shadowOpaqueVert = createShaderModule(shaderPaths[15]);
        shadowFrag = createShaderModule(shaderPaths[
            resourceOwner_.bindlessMaterialsEnabled ? 14U : 13U]);
        if (deviceOwner_.info.computeSubgroupBallot) {
            cullSubgroupCompute = createShaderModule(shaderPaths[10]);
        }
        const std::array<VkDescriptorSetLayout, 2> lightingLayouts{
            resourceOwner_.sceneSetLayout, resourceOwner_.lightingSetLayout};
        const VkPushConstantRange lightAssignmentPushRange{
            VK_SHADER_STAGE_COMPUTE_BIT, 0U,
            sizeof(LightAssignmentPushConstants)};
        VkPipelineLayoutCreateInfo lightAssignmentLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lightAssignmentLayoutInfo.setLayoutCount =
            static_cast<std::uint32_t>(lightingLayouts.size());
        lightAssignmentLayoutInfo.pSetLayouts = lightingLayouts.data();
        lightAssignmentLayoutInfo.pushConstantRangeCount = 1U;
        lightAssignmentLayoutInfo.pPushConstantRanges =
            &lightAssignmentPushRange;
        checkVk(vkCreatePipelineLayout(
                    deviceOwner_.device, &lightAssignmentLayoutInfo, nullptr,
                    &pipelines.lightAssignmentLayout),
                "vkCreatePipelineLayout light assignment");
        VkComputePipelineCreateInfo lightAssignmentInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        lightAssignmentInfo.stage = shaderStage(
            VK_SHADER_STAGE_COMPUTE_BIT, lightAssignmentCompute);
        lightAssignmentInfo.layout = pipelines.lightAssignmentLayout;
        checkVk(vkCreateComputePipelines(
                    deviceOwner_.device, pipelineOwner_.cache, 1U,
                    &lightAssignmentInfo, nullptr,
                    &pipelines.lightAssignment),
                "vkCreateComputePipelines light assignment");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                      handleToUint64(pipelines.lightAssignmentLayout),
                      "Light Assignment Pipeline Layout");
        setObjectName(VK_OBJECT_TYPE_PIPELINE,
                      handleToUint64(pipelines.lightAssignment),
                      "Light Assignment Pipeline");
        VkPipelineLayoutCreateInfo cullLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        cullLayoutInfo.setLayoutCount = 1;
        cullLayoutInfo.pSetLayouts = &resourceOwner_.cullSetLayout;
        checkVk(vkCreatePipelineLayout(deviceOwner_.device, &cullLayoutInfo, nullptr,
                                       &pipelines.cullLayout),
                "vkCreatePipelineLayout scene cull");
        VkComputePipelineCreateInfo cullInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cullInfo.stage = shaderStage(VK_SHADER_STAGE_COMPUTE_BIT, cullCompute);
        cullInfo.layout = pipelines.cullLayout;
        checkVk(vkCreateComputePipelines(deviceOwner_.device, pipelineOwner_.cache,
                                         1, &cullInfo, nullptr, &pipelines.cull),
                "vkCreateComputePipelines scene cull");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                      handleToUint64(pipelines.cullLayout),
                      "Scene Cull Pipeline Layout");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.cull),
                      "Scene Cull Pipeline");
        if (cullSubgroupCompute != VK_NULL_HANDLE) {
            cullInfo.stage =
                shaderStage(VK_SHADER_STAGE_COMPUTE_BIT,
                            cullSubgroupCompute);
            checkVk(vkCreateComputePipelines(
                        deviceOwner_.device, pipelineOwner_.cache, 1,
                        &cullInfo, nullptr, &pipelines.cullSubgroup),
                    "vkCreateComputePipelines subgroup scene cull");
            setObjectName(
                VK_OBJECT_TYPE_PIPELINE,
                handleToUint64(pipelines.cullSubgroup),
                "Subgroup Scene Cull Pipeline");
        }
        VkPushConstantRange depthPyramidPushRange{};
        depthPyramidPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        depthPyramidPushRange.size = sizeof(DepthPyramidPushConstants);
        VkPipelineLayoutCreateInfo depthPyramidLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        depthPyramidLayoutInfo.setLayoutCount = 1;
        depthPyramidLayoutInfo.pSetLayouts =
            &resourceOwner_.depthPyramidSetLayout;
        depthPyramidLayoutInfo.pushConstantRangeCount = 1;
        depthPyramidLayoutInfo.pPushConstantRanges =
            &depthPyramidPushRange;
        checkVk(vkCreatePipelineLayout(
                    deviceOwner_.device, &depthPyramidLayoutInfo, nullptr,
                    &pipelines.depthPyramidLayout),
                "vkCreatePipelineLayout depth pyramid");
        VkComputePipelineCreateInfo depthPyramidInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        depthPyramidInfo.stage =
            shaderStage(VK_SHADER_STAGE_COMPUTE_BIT, depthPyramidCompute);
        depthPyramidInfo.layout = pipelines.depthPyramidLayout;
        checkVk(vkCreateComputePipelines(
                    deviceOwner_.device, pipelineOwner_.cache, 1,
                    &depthPyramidInfo, nullptr, &pipelines.depthPyramid),
                "vkCreateComputePipelines depth pyramid");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                      handleToUint64(pipelines.depthPyramidLayout),
                      "Depth Pyramid Pipeline Layout");
        setObjectName(VK_OBJECT_TYPE_PIPELINE,
                      handleToUint64(pipelines.depthPyramid),
                      "Depth Pyramid Pipeline");

        std::array<VkPipelineShaderStageCreateInfo, 2> sceneStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, sceneVert), shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, sceneFrag)};
        std::array<VkVertexInputBindingDescription, 1> bindings{};
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(GpuVertex);
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 4> attributes{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)};
        attributes[1] = {1, 0, VK_FORMAT_R16G16B16A16_SNORM, offsetof(GpuVertex, normal)};
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)};
        attributes[3] = {3, 0, VK_FORMAT_R16G16B16A16_SNORM, offsetof(GpuVertex, tangent)};
        std::array<VkVertexInputAttributeDescription, 2> depthAttributes{};
        depthAttributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)};
        depthAttributes[1] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)};

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineVertexInputStateCreateInfo depthVertexInput = vertexInput;
        depthVertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(depthAttributes.size());
        depthVertexInput.pVertexAttributeDescriptions = depthAttributes.data();
        VkPipelineVertexInputStateCreateInfo opaqueShadowVertexInput =
            depthVertexInput;
        opaqueShadowVertexInput.vertexAttributeDescriptionCount = 1U;
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
        depthPrepassDepth.depthCompareOp = VK_COMPARE_OP_GREATER;
        VkPipelineDepthStencilStateCreateInfo sceneDepth = depthPrepassDepth;
        sceneDepth.depthWriteEnable = VK_FALSE;
        sceneDepth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

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

        const auto makeGraphicsPipelineInfo = [&](const VkPipelineRenderingCreateInfo& rendering,
                                                  const auto& stages,
                                                  const VkPipelineVertexInputStateCreateInfo& vertexInputState,
                                                  const VkPipelineRasterizationStateCreateInfo& rasterizerState,
                                                  const VkPipelineDepthStencilStateCreateInfo& depthState,
                                                  const VkPipelineColorBlendStateCreateInfo& blendState,
                                                  const VkPipelineLayout layout) {
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.pNext = &rendering;
            info.stageCount = static_cast<std::uint32_t>(stages.size());
            info.pStages = stages.data();
            info.pVertexInputState = &vertexInputState;
            info.pInputAssemblyState = &inputAssembly;
            info.pViewportState = &viewportState;
            info.pRasterizationState = &rasterizerState;
            info.pMultisampleState = &multisample;
            info.pDepthStencilState = &depthState;
            info.pColorBlendState = &blendState;
            info.pDynamicState = &dynamic;
            info.layout = layout;
            return info;
        };

        const std::array<VkDescriptorSetLayout, 2> sceneLayouts{
            resourceOwner_.sceneSetLayout, resourceOwner_.lightingSetLayout};
        VkPushConstantRange shadowPushRange{};
        shadowPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        shadowPushRange.size = sizeof(ShadowPushConstants);
        VkPipelineLayoutCreateInfo sceneLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        sceneLayoutInfo.setLayoutCount =
            static_cast<std::uint32_t>(sceneLayouts.size());
        sceneLayoutInfo.pSetLayouts = sceneLayouts.data();
        sceneLayoutInfo.pushConstantRangeCount = 1U;
        sceneLayoutInfo.pPushConstantRanges = &shadowPushRange;
        checkVk(vkCreatePipelineLayout(deviceOwner_.device, &sceneLayoutInfo, nullptr, &pipelines.sceneLayout), "vkCreatePipelineLayout scene");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, handleToUint64(pipelines.sceneLayout), "Scene Pipeline Layout");

        VkPipelineRenderingCreateInfo sceneRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        sceneRendering.colorAttachmentCount = 1;
        sceneRendering.pColorAttachmentFormats = &resourceOwner_.hdr.format;
        sceneRendering.depthAttachmentFormat = resourceOwner_.depth.format;

        const std::array<VkPipelineShaderStageCreateInfo, 1>
            opaqueDepthPrepassStages{
                shaderStage(
                    VK_SHADER_STAGE_VERTEX_BIT, depthPrepassOpaqueVert)};
        const std::array<VkPipelineShaderStageCreateInfo, 2> depthPrepassStages{
            shaderStage(VK_SHADER_STAGE_VERTEX_BIT, depthPrepassVert),
            shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, shadowFrag)};
        VkPipelineRenderingCreateInfo depthPrepassRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        depthPrepassRendering.colorAttachmentCount = 0;
        depthPrepassRendering.depthAttachmentFormat = resourceOwner_.depth.format;
        const VkGraphicsPipelineCreateInfo depthPrepassInfo =
            makeGraphicsPipelineInfo(
                depthPrepassRendering, depthPrepassStages, depthVertexInput,
                rasterizer, depthPrepassDepth, noColorBlend,
                pipelines.sceneLayout);
        const VkGraphicsPipelineCreateInfo depthPrepassOpaqueInfo =
            makeGraphicsPipelineInfo(
                depthPrepassRendering, opaqueDepthPrepassStages,
                opaqueShadowVertexInput, rasterizer, depthPrepassDepth,
                noColorBlend, pipelines.sceneLayout);
        VkGraphicsPipelineCreateInfo sceneInfo = makeGraphicsPipelineInfo(
            sceneRendering, sceneStages, vertexInput, rasterizer, sceneDepth,
            blend, pipelines.sceneLayout);
        VkGraphicsPipelineCreateInfo sceneNoPrepassInfo = sceneInfo;
        sceneNoPrepassInfo.pDepthStencilState = &depthPrepassDepth;
        const std::array<VkGraphicsPipelineCreateInfo, 4> scenePipelineInfos{
            depthPrepassInfo, depthPrepassOpaqueInfo, sceneInfo,
            sceneNoPrepassInfo};
        std::array<VkPipeline, 4> scenePipelineHandles{};
        const VkResult scenePipelineResult = vkCreateGraphicsPipelines(
            deviceOwner_.device, pipelineOwner_.cache,
            static_cast<std::uint32_t>(scenePipelineInfos.size()),
            scenePipelineInfos.data(), nullptr, scenePipelineHandles.data());
        pipelines.depthPrepass = scenePipelineHandles[0];
        pipelines.depthPrepassOpaque = scenePipelineHandles[1];
        pipelines.scene = scenePipelineHandles[2];
        pipelines.sceneNoPrepass = scenePipelineHandles[3];
        checkVk(scenePipelineResult, "vkCreateGraphicsPipelines scene set");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.depthPrepass), "Depth Prepass Pipeline");
        setObjectName(
            VK_OBJECT_TYPE_PIPELINE,
            handleToUint64(pipelines.depthPrepassOpaque),
            "Opaque Depth Prepass Pipeline");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.scene), "HDR Scene Pipeline");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.sceneNoPrepass), "HDR Scene Pipeline No Prepass");
        VkPipelineLayoutCreateInfo shadowLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        shadowLayoutInfo.setLayoutCount =
            static_cast<std::uint32_t>(sceneLayouts.size());
        shadowLayoutInfo.pSetLayouts = sceneLayouts.data();
        shadowLayoutInfo.pushConstantRangeCount = 1U;
        shadowLayoutInfo.pPushConstantRanges = &shadowPushRange;
        checkVk(vkCreatePipelineLayout(
                    deviceOwner_.device, &shadowLayoutInfo, nullptr,
                    &pipelines.shadowLayout),
                "vkCreatePipelineLayout shadow");
        setObjectName(
            VK_OBJECT_TYPE_PIPELINE_LAYOUT,
            handleToUint64(pipelines.shadowLayout),
            "Shadow Pipeline Layout");
        const std::array<VkPipelineShaderStageCreateInfo, 1>
            opaqueShadowStages{
                shaderStage(VK_SHADER_STAGE_VERTEX_BIT, shadowOpaqueVert)};
        const std::array<VkPipelineShaderStageCreateInfo, 2> shadowStages{
            shaderStage(VK_SHADER_STAGE_VERTEX_BIT, shadowVert),
            shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, shadowFrag)};
        VkPipelineRenderingCreateInfo shadowRendering{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        shadowRendering.depthAttachmentFormat =
            resourceOwner_.shadowAtlas.format;
        VkPipelineRasterizationStateCreateInfo shadowRasterizer = rasterizer;
        shadowRasterizer.depthBiasEnable = VK_TRUE;
        shadowRasterizer.depthBiasConstantFactor = -1.25F;
        shadowRasterizer.depthBiasSlopeFactor = -1.75F;
        const std::array<VkGraphicsPipelineCreateInfo, 2> shadowInfos{
            makeGraphicsPipelineInfo(
                shadowRendering, shadowStages, depthVertexInput,
                shadowRasterizer, depthPrepassDepth, noColorBlend,
                pipelines.shadowLayout),
            makeGraphicsPipelineInfo(
                shadowRendering, opaqueShadowStages, opaqueShadowVertexInput,
                shadowRasterizer, depthPrepassDepth, noColorBlend,
                pipelines.shadowLayout)};
        std::array<VkPipeline, 2> shadowHandles{};
        const VkResult shadowResult = vkCreateGraphicsPipelines(
            deviceOwner_.device, pipelineOwner_.cache,
            static_cast<std::uint32_t>(shadowInfos.size()),
            shadowInfos.data(), nullptr, shadowHandles.data());
        pipelines.shadow = shadowHandles[0];
        pipelines.shadowOpaque = shadowHandles[1];
        checkVk(shadowResult, "vkCreateGraphicsPipelines shadow set");
        setObjectName(VK_OBJECT_TYPE_PIPELINE,
                      handleToUint64(pipelines.shadow),
                      "Alpha-Masked Shadow Atlas Pipeline");
        setObjectName(VK_OBJECT_TYPE_PIPELINE,
                      handleToUint64(pipelines.shadowOpaque),
                      "Opaque Shadow Atlas Pipeline");

        std::array<VkPipelineShaderStageCreateInfo, 2> tonemapStages{shaderStage(VK_SHADER_STAGE_VERTEX_BIT, tonemapVert), shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, tonemapFrag)};
        VkPipelineVertexInputStateCreateInfo emptyVertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo noDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo noCullRasterizer = rasterizer;
        noCullRasterizer.cullMode = VK_CULL_MODE_NONE;
    const std::array<VkPipelineShaderStageCreateInfo, 2> atmosphereStages{
        shaderStage(VK_SHADER_STAGE_VERTEX_BIT, tonemapVert),
        shaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, atmosphereFrag)};
    VkGraphicsPipelineCreateInfo atmosphereInfo = makeGraphicsPipelineInfo(
        sceneRendering, atmosphereStages, emptyVertexInput, noCullRasterizer,
        noDepth, blend, pipelines.sceneLayout);
    checkVk(vkCreateGraphicsPipelines(deviceOwner_.device, pipelineOwner_.cache,
                                      1U, &atmosphereInfo, nullptr,
                                      &pipelines.atmosphere),
            "vkCreateGraphicsPipelines atmosphere");
    setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.atmosphere),
                  "Atmosphere Pipeline");

    VkPushConstantRange tonemapPushRange{};
        tonemapPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        tonemapPushRange.offset = 0;
        tonemapPushRange.size = sizeof(TonemapPushConstants);

        VkPipelineLayoutCreateInfo tonemapLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        tonemapLayoutInfo.setLayoutCount = 1;
        tonemapLayoutInfo.pSetLayouts = &resourceOwner_.tonemapSetLayout;
        tonemapLayoutInfo.pushConstantRangeCount = 1;
        tonemapLayoutInfo.pPushConstantRanges = &tonemapPushRange;
        checkVk(vkCreatePipelineLayout(deviceOwner_.device, &tonemapLayoutInfo, nullptr, &pipelines.tonemapLayout), "vkCreatePipelineLayout tonemap");
        setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, handleToUint64(pipelines.tonemapLayout), "Tonemap Pipeline Layout");

        VkPipelineRenderingCreateInfo tonemapRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        tonemapRendering.colorAttachmentCount = 1;
        tonemapRendering.pColorAttachmentFormats = &swapchainOwner_.format;

        VkGraphicsPipelineCreateInfo tonemapInfo = makeGraphicsPipelineInfo(tonemapRendering, tonemapStages, emptyVertexInput, noCullRasterizer, noDepth, blend, pipelines.tonemapLayout);
        checkVk(vkCreateGraphicsPipelines(deviceOwner_.device, pipelineOwner_.cache, 1, &tonemapInfo, nullptr, &pipelines.tonemap), "vkCreateGraphicsPipelines tonemap");
        setObjectName(VK_OBJECT_TYPE_PIPELINE, handleToUint64(pipelines.tonemap), "Tonemap Pipeline");
    } catch (...) {
        if (shadowFrag != VK_NULL_HANDLE) {
            vkDestroyShaderModule(deviceOwner_.device, shadowFrag, nullptr);
        }
        if (shadowOpaqueVert != VK_NULL_HANDLE) {
            vkDestroyShaderModule(
                deviceOwner_.device, shadowOpaqueVert, nullptr);
        }
        if (shadowVert != VK_NULL_HANDLE) {
            vkDestroyShaderModule(deviceOwner_.device, shadowVert, nullptr);
        }
        if (lightAssignmentCompute != VK_NULL_HANDLE) {
            vkDestroyShaderModule(deviceOwner_.device,
                                  lightAssignmentCompute, nullptr);
        }
        if (depthPyramidCompute != VK_NULL_HANDLE) {
            vkDestroyShaderModule(deviceOwner_.device, depthPyramidCompute,
                                  nullptr);
        }
        if (cullSubgroupCompute != VK_NULL_HANDLE) {
            vkDestroyShaderModule(deviceOwner_.device,
                                  cullSubgroupCompute, nullptr);
        }
        if (cullCompute != VK_NULL_HANDLE) {
            vkDestroyShaderModule(deviceOwner_.device, cullCompute, nullptr);
        }
        if (atmosphereFrag != VK_NULL_HANDLE) {
      vkDestroyShaderModule(deviceOwner_.device, atmosphereFrag, nullptr);
    }
        if (depthPrepassOpaqueVert != VK_NULL_HANDLE) {
            vkDestroyShaderModule(
                deviceOwner_.device, depthPrepassOpaqueVert, nullptr);
        }
    if (depthPrepassVert != VK_NULL_HANDLE) { vkDestroyShaderModule(deviceOwner_.device, depthPrepassVert, nullptr); }
        if (tonemapFrag != VK_NULL_HANDLE) { vkDestroyShaderModule(deviceOwner_.device, tonemapFrag, nullptr); }
        if (tonemapVert != VK_NULL_HANDLE) { vkDestroyShaderModule(deviceOwner_.device, tonemapVert, nullptr); }
        if (sceneFrag != VK_NULL_HANDLE) { vkDestroyShaderModule(deviceOwner_.device, sceneFrag, nullptr); }
        if (sceneVert != VK_NULL_HANDLE) { vkDestroyShaderModule(deviceOwner_.device, sceneVert, nullptr); }
        destroyPipelineSet(pipelines);
        throw;
    }

  vkDestroyShaderModule(deviceOwner_.device, atmosphereFrag, nullptr);
  vkDestroyShaderModule(deviceOwner_.device, shadowFrag, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, shadowOpaqueVert, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, shadowVert, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, lightAssignmentCompute, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, depthPyramidCompute, nullptr);
    if (cullSubgroupCompute != VK_NULL_HANDLE) {
        vkDestroyShaderModule(deviceOwner_.device, cullSubgroupCompute,
                              nullptr);
    }
    vkDestroyShaderModule(deviceOwner_.device, cullCompute, nullptr);
    vkDestroyShaderModule(
        deviceOwner_.device, depthPrepassOpaqueVert, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, depthPrepassVert, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, tonemapFrag, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, tonemapVert, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, sceneFrag, nullptr);
    vkDestroyShaderModule(deviceOwner_.device, sceneVert, nullptr);
    return pipelines;
}

void VulkanRenderer::Impl::createPipelines() {
    PipelineSet pipelines = buildPipelineSet();
    installPipelineSet(pipelines);
    refreshShaderWriteTimes();
}

void VulkanRenderer::Impl::destroyPipelineSet(PipelineSet& pipelines) const {
    if (pipelines.shadowOpaque != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.shadowOpaque, nullptr); pipelines.shadowOpaque = VK_NULL_HANDLE; }
    if (pipelines.shadow != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.shadow, nullptr); pipelines.shadow = VK_NULL_HANDLE; }
    if (pipelines.shadowLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(deviceOwner_.device, pipelines.shadowLayout, nullptr); pipelines.shadowLayout = VK_NULL_HANDLE; }
    if (pipelines.lightAssignment != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.lightAssignment, nullptr); pipelines.lightAssignment = VK_NULL_HANDLE; }
    if (pipelines.lightAssignmentLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(deviceOwner_.device, pipelines.lightAssignmentLayout, nullptr); pipelines.lightAssignmentLayout = VK_NULL_HANDLE; }
    if (pipelines.depthPyramid != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.depthPyramid, nullptr); pipelines.depthPyramid = VK_NULL_HANDLE; }
    if (pipelines.depthPyramidLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(deviceOwner_.device, pipelines.depthPyramidLayout, nullptr); pipelines.depthPyramidLayout = VK_NULL_HANDLE; }
    if (pipelines.cullSubgroup != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.cullSubgroup, nullptr); pipelines.cullSubgroup = VK_NULL_HANDLE; }
    if (pipelines.cull != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.cull, nullptr); pipelines.cull = VK_NULL_HANDLE; }
    if (pipelines.cullLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(deviceOwner_.device, pipelines.cullLayout, nullptr); pipelines.cullLayout = VK_NULL_HANDLE; }
    if (pipelines.atmosphere != VK_NULL_HANDLE) {
    vkDestroyPipeline(deviceOwner_.device, pipelines.atmosphere, nullptr);
    pipelines.atmosphere = VK_NULL_HANDLE;
  }
  if (pipelines.tonemap != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.tonemap, nullptr); pipelines.tonemap = VK_NULL_HANDLE; }
    if (pipelines.tonemapLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(deviceOwner_.device, pipelines.tonemapLayout, nullptr); pipelines.tonemapLayout = VK_NULL_HANDLE; }
    if (pipelines.depthPrepassOpaque != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.depthPrepassOpaque, nullptr); pipelines.depthPrepassOpaque = VK_NULL_HANDLE; }
    if (pipelines.depthPrepass != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.depthPrepass, nullptr); pipelines.depthPrepass = VK_NULL_HANDLE; }
    if (pipelines.scene != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.scene, nullptr); pipelines.scene = VK_NULL_HANDLE; }
    if (pipelines.sceneNoPrepass != VK_NULL_HANDLE) { vkDestroyPipeline(deviceOwner_.device, pipelines.sceneNoPrepass, nullptr); pipelines.sceneNoPrepass = VK_NULL_HANDLE; }
    if (pipelines.sceneLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(deviceOwner_.device, pipelines.sceneLayout, nullptr); pipelines.sceneLayout = VK_NULL_HANDLE; }
}

VulkanRenderer::Impl::PipelineSet VulkanRenderer::Impl::detachActivePipelineSet() noexcept {
    PipelineSet pipelines{};
    pipelines.sceneLayout = pipelineOwner_.sceneLayout;
    pipelines.depthPrepass = pipelineOwner_.depthPrepass;
    pipelines.depthPrepassOpaque = pipelineOwner_.depthPrepassOpaque;
    pipelines.scene = pipelineOwner_.scene;
    pipelines.sceneNoPrepass = pipelineOwner_.sceneNoPrepass;
    pipelines.atmosphere = pipelineOwner_.atmosphere;
  pipelines.tonemapLayout = pipelineOwner_.tonemapLayout;
    pipelines.tonemap = pipelineOwner_.tonemap;
    pipelines.cullLayout = pipelineOwner_.cullLayout;
    pipelines.cull = pipelineOwner_.cull;
    pipelines.cullSubgroup = pipelineOwner_.cullSubgroup;
    pipelines.depthPyramidLayout = pipelineOwner_.depthPyramidLayout;
    pipelines.depthPyramid = pipelineOwner_.depthPyramid;
    pipelines.lightAssignmentLayout = pipelineOwner_.lightAssignmentLayout;
    pipelines.lightAssignment = pipelineOwner_.lightAssignment;
    pipelines.shadowLayout = pipelineOwner_.shadowLayout;
    pipelines.shadow = pipelineOwner_.shadow;
    pipelines.shadowOpaque = pipelineOwner_.shadowOpaque;

    pipelineOwner_.sceneLayout = VK_NULL_HANDLE;
    pipelineOwner_.depthPrepass = VK_NULL_HANDLE;
    pipelineOwner_.depthPrepassOpaque = VK_NULL_HANDLE;
    pipelineOwner_.scene = VK_NULL_HANDLE;
  pipelineOwner_.atmosphere = VK_NULL_HANDLE;
    pipelineOwner_.sceneNoPrepass = VK_NULL_HANDLE;
    pipelineOwner_.tonemapLayout = VK_NULL_HANDLE;
    pipelineOwner_.tonemap = VK_NULL_HANDLE;
    pipelineOwner_.cullLayout = VK_NULL_HANDLE;
    pipelineOwner_.cull = VK_NULL_HANDLE;
    pipelineOwner_.cullSubgroup = VK_NULL_HANDLE;
    pipelineOwner_.depthPyramidLayout = VK_NULL_HANDLE;
    pipelineOwner_.depthPyramid = VK_NULL_HANDLE;
    pipelineOwner_.lightAssignmentLayout = VK_NULL_HANDLE;
    pipelineOwner_.lightAssignment = VK_NULL_HANDLE;
    pipelineOwner_.shadowLayout = VK_NULL_HANDLE;
    pipelineOwner_.shadow = VK_NULL_HANDLE;
    pipelineOwner_.shadowOpaque = VK_NULL_HANDLE;
    return pipelines;
}

void VulkanRenderer::Impl::retireDeferredPipelineSets() {
    for (auto it = pipelineOwner_.retiredSets.begin(); it != pipelineOwner_.retiredSets.end();) {
        bool ready = true;
        for (const VkFence fence : it->completionFences) {
            if (fence == VK_NULL_HANDLE) {
                continue;
            }
            const VkResult status = vkGetFenceStatus(deviceOwner_.device, fence);
            if (status == VK_NOT_READY) {
                ready = false;
                break;
            }
            checkVk(status, "vkGetFenceStatus retired pipeline set");
        }
        if (ready) {
            destroyPipelineSet(it->pipelines);
            it = pipelineOwner_.retiredSets.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanRenderer::Impl::installPipelineSet(const PipelineSet& pipelines) {
    pipelineOwner_.sceneLayout = pipelines.sceneLayout;
    pipelineOwner_.depthPrepass = pipelines.depthPrepass;
    pipelineOwner_.depthPrepassOpaque = pipelines.depthPrepassOpaque;
    pipelineOwner_.scene = pipelines.scene;
    pipelineOwner_.sceneNoPrepass = pipelines.sceneNoPrepass;
  pipelineOwner_.atmosphere = pipelines.atmosphere;
    pipelineOwner_.tonemapLayout = pipelines.tonemapLayout;
    pipelineOwner_.tonemap = pipelines.tonemap;
    pipelineOwner_.cullLayout = pipelines.cullLayout;
    pipelineOwner_.cull = pipelines.cull;
    pipelineOwner_.cullSubgroup = pipelines.cullSubgroup;
    pipelineOwner_.depthPyramidLayout = pipelines.depthPyramidLayout;
    pipelineOwner_.depthPyramid = pipelines.depthPyramid;
    pipelineOwner_.lightAssignmentLayout = pipelines.lightAssignmentLayout;
    pipelineOwner_.lightAssignment = pipelines.lightAssignment;
    pipelineOwner_.shadowLayout = pipelines.shadowLayout;
    pipelineOwner_.shadow = pipelines.shadow;
    pipelineOwner_.shadowOpaque = pipelines.shadowOpaque;
}

void VulkanRenderer::Impl::refreshShaderWriteTimes() {
    const auto shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    for (std::size_t i = 0; i < shaderPaths.size(); ++i) {
        std::error_code error;
        pipelineOwner_.shaderWriteTimes[i] = std::filesystem::last_write_time(shaderPaths[i], error);
        if (error) {
            pipelineOwner_.shaderWriteTimes[i] = {};
        }
    }
}

bool VulkanRenderer::Impl::shaderFilesChanged() const {
    const auto shaderPaths = shaderSpirvPaths(config_.shaderDirectory);
    for (std::size_t i = 0; i < shaderPaths.size(); ++i) {
        std::error_code error;
        const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(shaderPaths[i], error);
        if (error) {
            return false;
        }
        if (writeTime != pipelineOwner_.shaderWriteTimes[i]) {
            return true;
        }
    }
    return false;
}

void VulkanRenderer::Impl::pollShaderHotReload(const double elapsedSeconds) {
    if (!config_.shaderHotReload || elapsedSeconds - pipelineOwner_.hotReloadLastCheckSeconds < pipelineOwner_.hotReloadRetryDelaySeconds) {
        return;
    }
    pipelineOwner_.hotReloadLastCheckSeconds = elapsedSeconds;
    if (!shaderFilesChanged()) {
        return;
    }

    logger()->info("Detected shader bytecode change; rebuilding graphics pipelines");
    PipelineSet nextPipelines{};
    try {
        nextPipelines = buildPipelineSet();
    } catch (const std::exception& e) {
        pipelineOwner_.hotReloadRetryDelaySeconds =
            std::min(pipelineOwner_.hotReloadRetryDelaySeconds * 2.0, kShaderHotReloadMaxRetryDelaySeconds);
        logger()->warn("Shader hot reload failed; keeping existing pipelines and "
                   "retrying in {:.1f}s: {}",
                       pipelineOwner_.hotReloadRetryDelaySeconds, e.what());
        return;
    }

    PipelineSet oldPipelines{};
    oldPipelines.sceneLayout = pipelineOwner_.sceneLayout;
    oldPipelines.depthPrepass = pipelineOwner_.depthPrepass;
    oldPipelines.scene = pipelineOwner_.scene;
    oldPipelines.sceneNoPrepass = pipelineOwner_.sceneNoPrepass;
    oldPipelines.tonemapLayout = pipelineOwner_.tonemapLayout;
    oldPipelines.tonemap = pipelineOwner_.tonemap;

    RetiredPipelineSet retired{};
    retired.pipelines = oldPipelines;
    for (std::size_t index = 0; index < frameOwner_.frames.size(); ++index) {
        retired.completionFences[index] = frameOwner_.frames[index].submittedOnce ? frameOwner_.frames[index].inFlight : VK_NULL_HANDLE;
    }
    try {
        pipelineOwner_.retiredSets.push_back(retired);
    } catch (...) {
        destroyPipelineSet(nextPipelines);
        throw;
    }
    installPipelineSet(nextPipelines);
    pipelineOwner_.hotReloadRetryDelaySeconds = kShaderHotReloadInitialRetryDelaySeconds;
    refreshShaderWriteTimes();
    logger()->info("Reloaded graphics pipelines from updated shader bytecode; "
                 "retiring previous set after tracked frame fences signal");
}

VkShaderModule VulkanRenderer::Impl::createShaderModule(const std::filesystem::path& path) const {
    const std::vector<std::uint32_t> words = readSpirvWords(path);

    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = words.size() * sizeof(std::uint32_t);
    createInfo.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const std::string operation = "vkCreateShaderModule " + path.string();
    checkVk(vkCreateShaderModule(deviceOwner_.device, &createInfo, nullptr, &module), operation.c_str());
    return module;
}

} // namespace ve
