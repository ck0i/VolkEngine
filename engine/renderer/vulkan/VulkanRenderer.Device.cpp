#include "renderer/vulkan/VulkanRendererImpl.hpp"

namespace ve {

void VulkanRenderer::Impl::createInstance() {
    deviceOwner_.validationEnabled = config_.validation && validationLayerAvailable();
    deviceOwner_.info.validationEnabled = deviceOwner_.validationEnabled;
    if (config_.validation && !deviceOwner_.validationEnabled) {
        if (config_.requireValidation) {
            throw std::runtime_error(
                "Vulkan validation is required, but VK_LAYER_KHRONOS_validation is unavailable");
        }
        logger()->warn("Validation requested but VK_LAYER_KHRONOS_validation is not available");
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = config_.applicationName.c_str();
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "VolkEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    auto extensions = requiredInstanceExtensions();
    deviceOwner_.debugUtilsEnabled = instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (config_.requireValidation && !deviceOwner_.debugUtilsEnabled) {
        throw std::runtime_error(
            "Vulkan validation is required, but VK_EXT_debug_utils is unavailable");
    }
    const bool validationFeaturesEnabled =
        deviceOwner_.validationEnabled && instanceExtensionAvailable(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    if (config_.requireValidation && !validationFeaturesEnabled) {
        throw std::runtime_error(
            "Vulkan validation is required, but VK_EXT_validation_features is unavailable");
    }
    deviceOwner_.info.debugMarkers = deviceOwner_.debugUtilsEnabled;
    if (deviceOwner_.debugUtilsEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (validationFeaturesEnabled) {
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerCreateInfo(&deviceOwner_.validationMessages);
    constexpr std::array<VkValidationFeatureEnableEXT, 1> enabledValidationFeatures{
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount =
        static_cast<std::uint32_t>(enabledValidationFeatures.size());
    validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures.data();
    if (deviceOwner_.validationEnabled) {
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
        debugInfo.pNext = validationFeaturesEnabled ? &validationFeatures : nullptr;
        createInfo.pNext = &debugInfo;
    }

    checkVk(vkCreateInstance(&createInfo, nullptr, &deviceOwner_.instance), "vkCreateInstance");
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
    if (!deviceOwner_.validationEnabled || !deviceOwner_.debugUtilsEnabled) { return; }
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(deviceOwner_.instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!create) {
        throw std::runtime_error("vkCreateDebugUtilsMessengerEXT is unavailable");
    }
    VkDebugUtilsMessengerCreateInfoEXT createInfo = debugMessengerCreateInfo(&deviceOwner_.validationMessages);
    checkVk(create(deviceOwner_.instance, &createInfo, nullptr, &deviceOwner_.debugMessenger), "vkCreateDebugUtilsMessengerEXT");
}

void VulkanRenderer::Impl::loadDebugUtils() {
    if (!deviceOwner_.debugUtilsEnabled) { return; }
    deviceOwner_.setDebugObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(deviceOwner_.device, "vkSetDebugUtilsObjectNameEXT"));
    deviceOwner_.beginDebugLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetDeviceProcAddr(deviceOwner_.device, "vkCmdBeginDebugUtilsLabelEXT"));
    deviceOwner_.endDebugLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(deviceOwner_.device, "vkCmdEndDebugUtilsLabelEXT"));
    setObjectName(VK_OBJECT_TYPE_DEVICE, handleToUint64(deviceOwner_.device), "VolkEngine Logical Device");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(deviceOwner_.graphicsQueue), "Graphics Queue");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(deviceOwner_.presentQueue), "Present Queue");
    setObjectName(VK_OBJECT_TYPE_QUEUE, handleToUint64(deviceOwner_.transferQueue), "Transfer Queue");
}

void VulkanRenderer::Impl::setObjectName(const VkObjectType objectType, const std::uint64_t objectHandle, const std::string_view name) const {
    if (!deviceOwner_.setDebugObjectName || objectHandle == 0U || name.empty()) {
        return;
    }
    const std::string ownedName{name};
    VkDebugUtilsObjectNameInfoEXT nameInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    nameInfo.objectType = objectType;
    nameInfo.objectHandle = objectHandle;
    nameInfo.pObjectName = ownedName.c_str();
    const VkResult result = deviceOwner_.setDebugObjectName(deviceOwner_.device, &nameInfo);
    if (result != VK_SUCCESS) {
        logger()->warn("vkSetDebugUtilsObjectNameEXT failed for {} with {}", ownedName, static_cast<int>(result));
    }
}

void VulkanRenderer::Impl::beginDebugLabel(const VkCommandBuffer commandBuffer, const char* name, const std::array<float, 4>& color) const {
    if (!deviceOwner_.beginDebugLabel || name == nullptr || name[0] == '\0') {
        return;
    }
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    std::ranges::copy(color, label.color);
    deviceOwner_.beginDebugLabel(commandBuffer, &label);
}

void VulkanRenderer::Impl::endDebugLabel(const VkCommandBuffer commandBuffer) const {
    if (deviceOwner_.endDebugLabel) {
        deviceOwner_.endDebugLabel(commandBuffer);
    }
}

VulkanRenderer::Impl::DebugLabelScope::DebugLabelScope(const VulkanRenderer::Impl& renderer,
                                                 const VkCommandBuffer commandBuffer,
                                                 const char* name,
                                                 const std::array<float, 4>& color) noexcept
    : renderer_(&renderer), commandBuffer_(commandBuffer),
      active_(renderer.deviceOwner_.beginDebugLabel != nullptr &&
              name != nullptr && name[0] != '\0') {
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
    window_.createSurface(deviceOwner_.instance, &swapchainOwner_.surface);
}

void VulkanRenderer::Impl::pickPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(deviceOwner_.instance, &deviceCount, nullptr), "vkEnumeratePhysicalDevices count");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan-capable physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(deviceOwner_.instance, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices data");

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

    deviceOwner_.physicalDevice = bestDevice;
    deviceOwner_.queueFamilies = findQueueFamilies(deviceOwner_.physicalDevice);
    vkGetPhysicalDeviceProperties(deviceOwner_.physicalDevice, &deviceOwner_.physicalDeviceProperties);
    vkGetPhysicalDeviceMemoryProperties(deviceOwner_.physicalDevice, &deviceOwner_.physicalDeviceMemoryProperties);
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(deviceOwner_.physicalDevice, &features2);
    deviceOwner_.info.backend = RenderBackend::Vulkan;
    deviceOwner_.info.adapterName = deviceOwner_.physicalDeviceProperties.deviceName;
    deviceOwner_.info.apiVersionMajor = VK_VERSION_MAJOR(deviceOwner_.physicalDeviceProperties.apiVersion);
    deviceOwner_.info.apiVersionMinor = VK_VERSION_MINOR(deviceOwner_.physicalDeviceProperties.apiVersion);
    deviceOwner_.info.apiVersionPatch = VK_VERSION_PATCH(deviceOwner_.physicalDeviceProperties.apiVersion);
    deviceOwner_.info.driverVersion = deviceOwner_.physicalDeviceProperties.driverVersion;
    deviceOwner_.info.vendorId = deviceOwner_.physicalDeviceProperties.vendorID;
    deviceOwner_.info.deviceId = deviceOwner_.physicalDeviceProperties.deviceID;
    deviceOwner_.info.maxImageDimension2D = deviceOwner_.physicalDeviceProperties.limits.maxImageDimension2D;
    deviceOwner_.info.discreteGpu = deviceOwner_.physicalDeviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    deviceOwner_.info.dynamicRendering = features13.dynamicRendering == VK_TRUE;
    deviceOwner_.info.synchronization2 = features13.synchronization2 == VK_TRUE;
    deviceOwner_.info.descriptorIndexing = features12.descriptorIndexing == VK_TRUE;
    deviceOwner_.info.bindlessSampledImagesSupported = supportsBindlessSampledImages(features12);
    deviceOwner_.info.multiDrawIndirect = features2.features.multiDrawIndirect == VK_TRUE;
    deviceOwner_.info.drawIndirectFirstInstance = features2.features.drawIndirectFirstInstance == VK_TRUE;
    deviceOwner_.info.samplerAnisotropy = features2.features.samplerAnisotropy == VK_TRUE;
    deviceOwner_.info.maxSamplerAnisotropy = deviceOwner_.info.samplerAnisotropy ? deviceOwner_.physicalDeviceProperties.limits.maxSamplerAnisotropy : 1.0f;
    deviceOwner_.info.maxDrawIndirectCount = deviceOwner_.physicalDeviceProperties.limits.maxDrawIndirectCount;
    logger()->info("Selected GPU: {} ({}, Vulkan {}.{}.{})",
                   deviceOwner_.physicalDeviceProperties.deviceName,
                   gpuClassName(deviceOwner_.info.discreteGpu),
                   VK_VERSION_MAJOR(deviceOwner_.physicalDeviceProperties.apiVersion),
                   VK_VERSION_MINOR(deviceOwner_.physicalDeviceProperties.apiVersion),
                   VK_VERSION_PATCH(deviceOwner_.physicalDeviceProperties.apiVersion));
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
        checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, swapchainOwner_.surface, &presentSupport), "vkGetPhysicalDeviceSurfaceSupportKHR");
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
    std::set<std::uint32_t> uniqueFamilies{deviceOwner_.queueFamilies.graphics.value(), deviceOwner_.queueFamilies.present.value(), deviceOwner_.queueFamilies.transfer.value()};
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
    vkGetPhysicalDeviceFeatures2(deviceOwner_.physicalDevice, &supportedFeatures2);
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
    const float deviceMaxSamplerAnisotropy = std::max(1.0f, deviceOwner_.physicalDeviceProperties.limits.maxSamplerAnisotropy);
    resourceOwner_.samplerAnisotropyEnabled = supportedFeatures2.features.samplerAnisotropy == VK_TRUE && deviceMaxSamplerAnisotropy > 1.0f;
    resourceOwner_.maxSamplerAnisotropy = resourceOwner_.samplerAnisotropyEnabled ? std::min(deviceMaxSamplerAnisotropy, 16.0f) : 1.0f;
    if (resourceOwner_.samplerAnisotropyEnabled) {
        deviceFeatures.samplerAnisotropy = VK_TRUE;
    }
    deviceOwner_.multiDrawIndirectEnabled = supportedFeatures2.features.multiDrawIndirect == VK_TRUE;
    deviceOwner_.drawIndirectFirstInstanceEnabled = supportedFeatures2.features.drawIndirectFirstInstance == VK_TRUE;
    if (deviceOwner_.multiDrawIndirectEnabled) {
        deviceFeatures.multiDrawIndirect = VK_TRUE;
    }
    if (deviceOwner_.drawIndirectFirstInstanceEnabled) {
        deviceFeatures.drawIndirectFirstInstance = VK_TRUE;
    }
    indirectSceneDrawsEnabled_ = config_.indirectSceneDraws &&
                                  deviceOwner_.multiDrawIndirectEnabled && deviceOwner_.drawIndirectFirstInstanceEnabled &&
                                  deviceOwner_.physicalDeviceProperties.limits.maxDrawIndirectCount >=
                                      kBaseSceneMeshBatchOrder.size() + resourceOwner_.referenceAssets->scene.meshes.size();
    if (config_.indirectSceneDraws && !indirectSceneDrawsEnabled_) {
        std::string reason;
        const auto appendReason = [&reason](const std::string_view text) {
            if (!reason.empty()) {
                reason += ", ";
            }
            reason += text;
        };
        if (!deviceOwner_.multiDrawIndirectEnabled) {
            appendReason("multiDrawIndirect unsupported");
        }
        if (!deviceOwner_.drawIndirectFirstInstanceEnabled) {
            appendReason("drawIndirectFirstInstance unsupported");
        }
        const std::size_t requiredDrawCount =
            kBaseSceneMeshBatchOrder.size() + resourceOwner_.referenceAssets->scene.meshes.size();
        if (deviceOwner_.physicalDeviceProperties.limits.maxDrawIndirectCount < requiredDrawCount) {
            appendReason("maxDrawIndirectCount " + std::to_string(deviceOwner_.physicalDeviceProperties.limits.maxDrawIndirectCount)
                         + " < required " + std::to_string(requiredDrawCount));
        }
        logger()->warn("Indirect scene draws requested but disabled: {}; using direct indexed-instanced fallback", reason);
    }
    deviceOwner_.info.multiDrawIndirect = deviceOwner_.multiDrawIndirectEnabled;
    deviceOwner_.info.drawIndirectFirstInstance = deviceOwner_.drawIndirectFirstInstanceEnabled;
    deviceOwner_.info.indirectSceneDraws = indirectSceneDrawsEnabled_;
    deviceOwner_.info.samplerAnisotropy = resourceOwner_.samplerAnisotropyEnabled;
    deviceOwner_.info.maxSamplerAnisotropy = resourceOwner_.maxSamplerAnisotropy;

    deviceOwner_.info.descriptorIndexing = descriptorIndexingEnabled;
    deviceOwner_.info.bindlessSampledImagesSupported = bindlessSampledImagesEnabled;
    std::vector<const char*> enabledExtensions(kDeviceExtensions.begin(), kDeviceExtensions.end());
    if (deviceExtensionAvailable(deviceOwner_.physicalDevice, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
        enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        deviceOwner_.memoryBudgetEnabled = true;
        deviceOwner_.info.memoryBudget = true;
    }

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &features12;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();
    createInfo.pEnabledFeatures = &deviceFeatures;

    checkVk(vkCreateDevice(deviceOwner_.physicalDevice, &createInfo, nullptr, &deviceOwner_.device), "vkCreateDevice");
    vkGetDeviceQueue(deviceOwner_.device, deviceOwner_.queueFamilies.graphics.value(), 0, &deviceOwner_.graphicsQueue);
    vkGetDeviceQueue(deviceOwner_.device, deviceOwner_.queueFamilies.present.value(), 0, &deviceOwner_.presentQueue);
    vkGetDeviceQueue(deviceOwner_.device, deviceOwner_.queueFamilies.transfer.value(), 0, &deviceOwner_.transferQueue);
    deviceOwner_.info.transferUploadSync = deviceOwner_.transferQueue != deviceOwner_.graphicsQueue ? TransferUploadSyncMode::QueueSemaphore : TransferUploadSyncMode::SameQueueBarrier;
}

void VulkanRenderer::Impl::createAllocator() {
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = deviceOwner_.physicalDevice;
    allocatorInfo.device = deviceOwner_.device;
    allocatorInfo.instance = deviceOwner_.instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    if (deviceOwner_.memoryBudgetEnabled) {
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }

    checkVk(vmaCreateAllocator(&allocatorInfo, &deviceOwner_.allocator), "vmaCreateAllocator");
}

void VulkanRenderer::Impl::createCommandPools() {
    VkCommandPoolCreateInfo graphicsInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    graphicsInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    graphicsInfo.queueFamilyIndex = deviceOwner_.queueFamilies.graphics.value();
    checkVk(vkCreateCommandPool(deviceOwner_.device, &graphicsInfo, nullptr, &frameOwner_.graphicsCommandPool), "vkCreateCommandPool graphics");
    setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(frameOwner_.graphicsCommandPool), "Graphics Command Pool");

    VkCommandPoolCreateInfo transferInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    transferInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    transferInfo.queueFamilyIndex = deviceOwner_.queueFamilies.transfer.value();
    checkVk(vkCreateCommandPool(deviceOwner_.device, &transferInfo, nullptr, &frameOwner_.transferCommandPool), "vkCreateCommandPool transfer");
    setObjectName(VK_OBJECT_TYPE_COMMAND_POOL, handleToUint64(frameOwner_.transferCommandPool), "Transfer Command Pool");
}

} // namespace ve
