#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

void VulkanRenderer::Impl::createInstance() {
    validationEnabled_ = config_.validation && validationLayerAvailable();
    deviceInfo_.validationEnabled = validationEnabled_;
    if (config_.validation && !validationEnabled_) {
        logger()->warn("Validation requested but VK_LAYER_KHRONOS_validation is not available");
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = config_.applicationName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "VolkEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    auto extensions = requiredInstanceExtensions();
    debugUtilsEnabled_ = instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    deviceInfo_.debugMarkers = debugUtilsEnabled_;
    if (debugUtilsEnabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerCreateInfo();
    if (validationEnabled_) {
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
        createInfo.pNext = &debugInfo;
    }

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");
}

bool VulkanRenderer::Impl::validationLayerAvailable() const {
    std::uint32_t layerCount = 0;
    checkVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "vkEnumerateInstanceLayerProperties count");
    std::vector<VkLayerProperties> layers(layerCount);
    checkVk(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "vkEnumerateInstanceLayerProperties data");

    for (const char* required : kValidationLayers) {
        const bool found = std::ranges::any_of(layers, [required](const VkLayerProperties& layer) {
            return std::strcmp(required, layer.layerName) == 0;
        });
        if (!found) { return false; }
    }
    return true;
}

std::vector<const char*> VulkanRenderer::Impl::requiredInstanceExtensions() const {
    std::uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (!glfwExtensions) {
        throw std::runtime_error("GLFW did not report required Vulkan instance extensions");
    }
    return {glfwExtensions, glfwExtensions + glfwExtensionCount};
}

bool VulkanRenderer::Impl::instanceExtensionAvailable(const char* extensionName) const {
    std::uint32_t extensionCount = 0;
    checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr), "vkEnumerateInstanceExtensionProperties count");
    std::vector<VkExtensionProperties> extensions(extensionCount);
    checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data()), "vkEnumerateInstanceExtensionProperties data");
    return std::ranges::any_of(extensions, [extensionName](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, extensionName) == 0;
    });
}

void VulkanRenderer::Impl::createDebugMessenger() {
    if (!validationEnabled_ || !debugUtilsEnabled_) { return; }
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (!create) {
        throw std::runtime_error("vkCreateDebugUtilsMessengerEXT is unavailable");
    }
    VkDebugUtilsMessengerCreateInfoEXT createInfo = debugMessengerCreateInfo();
    checkVk(create(instance_, &createInfo, nullptr, &debugMessenger_), "vkCreateDebugUtilsMessengerEXT");
}

void VulkanRenderer::Impl::loadDebugUtils() {
    if (!debugUtilsEnabled_) { return; }
    vkSetDebugUtilsObjectNameEXT_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
    vkCmdBeginDebugUtilsLabelEXT_ = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdBeginDebugUtilsLabelEXT"));
    vkCmdEndDebugUtilsLabelEXT_ = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device_, "vkCmdEndDebugUtilsLabelEXT"));
    setObjectName(VK_OBJECT_TYPE_DEVICE, handleToUint64(device_), "VolkEngine Logical Device");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(graphicsQueue_), "Graphics Queue");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(presentQueue_), "Present Queue");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(transferQueue_), "Transfer Queue");
}

void VulkanRenderer::Impl::setObjectName(const VkObjectType objectType, const std::uint64_t objectHandle, const std::string_view name) const {
    if (!vkSetDebugUtilsObjectNameEXT_ || objectHandle == 0U || name.empty()) {
        return;
    }
    const std::string ownedName{name};
    VkDebugUtilsObjectNameInfoEXT nameInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    nameInfo.objectType = objectType;
    nameInfo.objectHandle = objectHandle;
    nameInfo.pObjectName = ownedName.c_str();
    const VkResult result = vkSetDebugUtilsObjectNameEXT_(device_, &nameInfo);
    if (result != VK_SUCCESS) {
        logger()->warn("vkSetDebugUtilsObjectNameEXT failed for {} with {}", ownedName, static_cast<int>(result));
    }
}

void VulkanRenderer::Impl::beginDebugLabel(const VkCommandBuffer commandBuffer, const char* name, const std::array<float, 4>& color) const {
    if (!vkCmdBeginDebugUtilsLabelEXT_ || name == nullptr || name[0] == '\0') {
        return;
    }
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    std::ranges::copy(color, label.color);
    vkCmdBeginDebugUtilsLabelEXT_(commandBuffer, &label);
}

void VulkanRenderer::Impl::endDebugLabel(const VkCommandBuffer commandBuffer) const {
    if (vkCmdEndDebugUtilsLabelEXT_) {
        vkCmdEndDebugUtilsLabelEXT_(commandBuffer);
    }
}

VulkanRenderer::Impl::DebugLabelScope::DebugLabelScope(const VulkanRenderer::Impl& renderer,
                                                 const VkCommandBuffer commandBuffer,
                                                 const char* name,
                                                 const std::array<float, 4>& color) noexcept
    : renderer_(&renderer), commandBuffer_(commandBuffer),
      active_(renderer.vkCmdBeginDebugUtilsLabelEXT_ != nullptr && name != nullptr && name[0] != '\0') {
    if (active_) {
        renderer_->beginDebugLabel(commandBuffer_, name, color);
    }
}

VulkanRenderer::Impl::DebugLabelScope::~DebugLabelScope() noexcept {
    if (active_ && renderer_ != nullptr) {
        renderer_->endDebugLabel(commandBuffer_);
    }
}

void VulkanRenderer::Impl::createSurface() {
    window_.createSurface(instance_, &surface_);
}

void VulkanRenderer::Impl::pickPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices count");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan-capable physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices data");

    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    int bestScore = std::numeric_limits<int>::min();
    std::vector<std::string> rejectionMessages;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        const DeviceSuitability suitability = evaluateDeviceSuitability(device);
        if (!suitability.suitable) {
            std::string message = properties.deviceName;
            message += ": ";
            for (std::size_t i = 0; i < suitability.reasons.size(); ++i) {
                if (i > 0U) { message += "; "; }
                message += suitability.reasons[i];
            }
            rejectionMessages.push_back(std::move(message));
            continue;
        }
        int score = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { score += 1000; }
        score += static_cast<int>(properties.limits.maxImageDimension2D);
        if (bestDevice == VK_NULL_HANDLE || score > bestScore) {
            bestDevice = device;
            bestScore = score;
        }
    }

    if (bestDevice == VK_NULL_HANDLE) {
        std::string message = "No suitable Vulkan physical device found";
        if (!rejectionMessages.empty()) {
            message += ":";
            for (const std::string& rejection : rejectionMessages) {
                message += "\n - ";
                message += rejection;
            }
        }
        throw std::runtime_error(message);
    }

    physicalDevice_ = bestDevice;
    queueFamilies_ = findQueueFamilies(physicalDevice_);
    vkGetPhysicalDeviceProperties(physicalDevice_, &physicalDeviceProperties_);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &physicalDeviceMemoryProperties_);
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features2);
    deviceInfo_.backend = RenderBackend::Vulkan;
    deviceInfo_.adapterName = physicalDeviceProperties_.deviceName;
    deviceInfo_.apiVersionMajor = VK_VERSION_MAJOR(physicalDeviceProperties_.apiVersion);
    deviceInfo_.apiVersionMinor = VK_VERSION_MINOR(physicalDeviceProperties_.apiVersion);
    deviceInfo_.apiVersionPatch = VK_VERSION_PATCH(physicalDeviceProperties_.apiVersion);
    deviceInfo_.maxImageDimension2D = physicalDeviceProperties_.limits.maxImageDimension2D;
    deviceInfo_.discreteGpu = physicalDeviceProperties_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    deviceInfo_.dynamicRendering = features13.dynamicRendering == VK_TRUE;
    deviceInfo_.synchronization2 = features13.synchronization2 == VK_TRUE;
    deviceInfo_.descriptorIndexing = features12.descriptorIndexing == VK_TRUE;
    deviceInfo_.bindlessSampledImagesSupported = supportsBindlessSampledImages(features12);
    deviceInfo_.multiDrawIndirect = features2.features.multiDrawIndirect == VK_TRUE;
    deviceInfo_.drawIndirectFirstInstance = features2.features.drawIndirectFirstInstance == VK_TRUE;
    deviceInfo_.samplerAnisotropy = features2.features.samplerAnisotropy == VK_TRUE;
    deviceInfo_.maxSamplerAnisotropy = deviceInfo_.samplerAnisotropy ? physicalDeviceProperties_.limits.maxSamplerAnisotropy : 1.0f;
    deviceInfo_.maxDrawIndirectCount = physicalDeviceProperties_.limits.maxDrawIndirectCount;
    logger()->info("Selected GPU: {} ({}, Vulkan {}.{}.{})",
                   physicalDeviceProperties_.deviceName,
                   gpuClassName(deviceInfo_.discreteGpu),
                   VK_VERSION_MAJOR(physicalDeviceProperties_.apiVersion),
                   VK_VERSION_MINOR(physicalDeviceProperties_.apiVersion),
                   VK_VERSION_PATCH(physicalDeviceProperties_.apiVersion));
}

bool VulkanRenderer::Impl::deviceExtensionAvailable(VkPhysicalDevice device, const char* extensionName) const {
    std::uint32_t extensionCount = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties count");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data()), "vkEnumerateDeviceExtensionProperties data");
    return std::ranges::any_of(available, [extensionName](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, extensionName) == 0;
    });
}

VulkanRenderer::Impl::DeviceSuitability VulkanRenderer::Impl::evaluateDeviceSuitability(VkPhysicalDevice device) const {
    DeviceSuitability result{};
    const auto reject = [&result](std::string reason) {
        result.suitable = false;
        result.reasons.push_back(std::move(reason));
    };

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        reject("Vulkan API version is below 1.3");
    }

    const QueueFamilies families = findQueueFamilies(device);
    if (!families.graphics.has_value()) { reject("missing graphics queue family"); }
    if (!families.present.has_value()) { reject("missing present queue family for the window surface"); }
    if (!families.transfer.has_value()) { reject("missing transfer queue family"); }

    std::uint32_t extensionCount = 0;
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties count");
    std::vector<VkExtensionProperties> available(extensionCount);
    checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data()), "vkEnumerateDeviceExtensionProperties data");
    std::set<std::string_view> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const VkExtensionProperties& extension : available) {
        required.erase(extension.extensionName);
    }
    for (const std::string_view missing : required) {
        reject("missing device extension " + std::string(missing));
    }

    if (required.empty()) {
        const SwapchainSupport swapchainSupport = querySwapchainSupport(device);
        if (swapchainSupport.formats.empty()) { reject("window surface exposes no swapchain formats"); }
        if (swapchainSupport.presentModes.empty()) { reject("window surface exposes no present modes"); }
        if ((swapchainSupport.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0U) {
            reject("window surface swapchain images cannot be color attachments");
        }
    }

    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(device, &features2);
    if (features13.dynamicRendering != VK_TRUE) { reject("missing Vulkan 1.3 dynamicRendering feature"); }
    if (features13.synchronization2 != VK_TRUE) { reject("missing Vulkan 1.3 synchronization2 feature"); }

    return result;
}

VulkanRenderer::Impl::QueueFamilies VulkanRenderer::Impl::findQueueFamilies(VkPhysicalDevice device) const {
    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, families.data());

    QueueFamilies result{};
    std::optional<std::uint32_t> firstGraphics;
    std::optional<std::uint32_t> firstPresent;
    std::optional<std::uint32_t> firstTransfer;
    std::optional<std::uint32_t> nonGraphicsTransfer;
    std::optional<std::uint32_t> transferOnly;
    for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
        const VkQueueFamilyProperties& family = families[i];
        const bool supportsGraphics = (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U;
        const bool supportsTransfer = (family.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0U;

        VkBool32 presentSupport = VK_FALSE;
        checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport), "vkGetPhysicalDeviceSurfaceSupportKHR");
        const bool supportsPresent = presentSupport == VK_TRUE;

        if (supportsGraphics && !firstGraphics.has_value()) {
            firstGraphics = i;
        }
        if (supportsPresent && !firstPresent.has_value()) {
            firstPresent = i;
        }
        if (supportsGraphics && supportsPresent && !result.graphics.has_value()) {
            result.graphics = i;
            result.present = i;
        }
        if (supportsTransfer) {
            if (!firstTransfer.has_value()) {
                firstTransfer = i;
            }
            if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U && !nonGraphicsTransfer.has_value()) {
                nonGraphicsTransfer = i;
            }
            if ((family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0U && !transferOnly.has_value()) {
                transferOnly = i;
            }
        }
    }
    if (!result.graphics.has_value()) {
        result.graphics = firstGraphics;
    }
    if (!result.present.has_value()) {
        result.present = firstPresent;
    }
    result.transfer = transferOnly.has_value() ? transferOnly : (nonGraphicsTransfer.has_value() ? nonGraphicsTransfer : firstTransfer);
    return result;
}

void VulkanRenderer::Impl::createLogicalDevice() {
    std::set<std::uint32_t> uniqueFamilies{queueFamilies_.graphics.value(), queueFamilies_.present.value(), queueFamilies_.transfer.value()};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const float queuePriority = 1.0f;
    for (std::uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;


    VkPhysicalDeviceVulkan12Features supportedFeatures12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 supportedFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supportedFeatures2.pNext = &supportedFeatures12;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures2);
    const bool descriptorIndexingEnabled = supportedFeatures12.descriptorIndexing == VK_TRUE;
    const bool bindlessSampledImagesEnabled = supportsBindlessSampledImages(supportedFeatures12);
    if (descriptorIndexingEnabled) {
        features12.descriptorIndexing = VK_TRUE;
    }
    if (bindlessSampledImagesEnabled) {
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    }
    VkPhysicalDeviceFeatures deviceFeatures{};
    const float deviceMaxSamplerAnisotropy = std::max(1.0f, physicalDeviceProperties_.limits.maxSamplerAnisotropy);
    samplerAnisotropyEnabled_ = supportedFeatures2.features.samplerAnisotropy == VK_TRUE && deviceMaxSamplerAnisotropy > 1.0f;
    maxSamplerAnisotropy_ = samplerAnisotropyEnabled_ ? std::min(deviceMaxSamplerAnisotropy, 16.0f) : 1.0f;
    if (samplerAnisotropyEnabled_) {
        deviceFeatures.samplerAnisotropy = VK_TRUE;
    }
    multiDrawIndirectEnabled_ = supportedFeatures2.features.multiDrawIndirect == VK_TRUE;
    drawIndirectFirstInstanceEnabled_ = supportedFeatures2.features.drawIndirectFirstInstance == VK_TRUE;
    if (multiDrawIndirectEnabled_) {
        deviceFeatures.multiDrawIndirect = VK_TRUE;
    }
    if (drawIndirectFirstInstanceEnabled_) {
        deviceFeatures.drawIndirectFirstInstance = VK_TRUE;
    }
    indirectSceneDrawsEnabled_ = config_.indirectSceneDraws &&
                                  multiDrawIndirectEnabled_ && drawIndirectFirstInstanceEnabled_ &&
                                  physicalDeviceProperties_.limits.maxDrawIndirectCount >= kSceneMeshBatchOrder.size();
    if (config_.indirectSceneDraws && !indirectSceneDrawsEnabled_) {
        std::string reason;
        const auto appendReason = [&reason](const std::string_view text) {
            if (!reason.empty()) {
                reason += ", ";
            }
            reason += text;
        };
        if (!multiDrawIndirectEnabled_) {
            appendReason("multiDrawIndirect unsupported");
        }
        if (!drawIndirectFirstInstanceEnabled_) {
            appendReason("drawIndirectFirstInstance unsupported");
        }
        if (physicalDeviceProperties_.limits.maxDrawIndirectCount < kSceneMeshBatchOrder.size()) {
            appendReason("maxDrawIndirectCount " + std::to_string(physicalDeviceProperties_.limits.maxDrawIndirectCount)
                         + " < required " + std::to_string(kSceneMeshBatchOrder.size()));
        }
        logger()->warn("Indirect scene draws requested but disabled: {}; using direct indexed-instanced fallback", reason);
    }
    deviceInfo_.multiDrawIndirect = multiDrawIndirectEnabled_;
    deviceInfo_.drawIndirectFirstInstance = drawIndirectFirstInstanceEnabled_;
    deviceInfo_.indirectSceneDraws = indirectSceneDrawsEnabled_;
    deviceInfo_.samplerAnisotropy = samplerAnisotropyEnabled_;
    deviceInfo_.maxSamplerAnisotropy = maxSamplerAnisotropy_;

    deviceInfo_.descriptorIndexing = descriptorIndexingEnabled;
    deviceInfo_.bindlessSampledImagesSupported = bindlessSampledImagesEnabled;
    std::vector<const char*> enabledExtensions(kDeviceExtensions.begin(), kDeviceExtensions.end());
    if (deviceExtensionAvailable(physicalDevice_, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        memoryBudgetEnabled_ = true;
        deviceInfo_.memoryBudget = true;
    }

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &features12;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamilies_.graphics.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.present.value(), 0, &presentQueue_);
    vkGetDeviceQueue(device_, queueFamilies_.transfer.value(), 0, &transferQueue_);
    deviceInfo_.transferUploadSync = transferQueue_ != graphicsQueue_ ? TransferUploadSyncMode::QueueSemaphore : TransferUploadSyncMode::SameQueueBarrier;
}

void VulkanRenderer::Impl::createAllocator() {
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = physicalDevice_;
    allocatorInfo.device = device_;
    allocatorInfo.instance = instance_;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    if (memoryBudgetEnabled_) {
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }

    checkVk(vmaCreateAllocator(&allocatorInfo, &allocator_), "vmaCreateAllocator");
}

void VulkanRenderer::Impl::createCommandPools() {
    VkCommandPoolCreateInfo graphicsInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    graphicsInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    graphicsInfo.queueFamilyIndex = queueFamilies_.graphics.value();
    checkVk(vkCreateCommandPool(device_, &graphicsInfo, nullptr, &graphicsCommandPool_), "vkCreateCommandPool graphics");
    setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(graphicsCommandPool_), "Graphics Command Pool");

    VkCommandPoolCreateInfo transferInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    transferInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    transferInfo.queueFamilyIndex = queueFamilies_.transfer.value();
    checkVk(vkCreateCommandPool(device_, &transferInfo, nullptr, &transferCommandPool_), "vkCreateCommandPool transfer");
    setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(transferCommandPool_), "Transfer Command Pool");
}

} // namespace ve
