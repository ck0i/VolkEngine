#include "core/Math.hpp"
#include "renderer/vulkan/VulkanRendererImpl.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

int gFailureCount = 0;
struct TestPendingUpload {
    VkSemaphore signalSemaphore = VK_NULL_HANDLE;
};

template <typename Handle>
Handle fakeHandle(const std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}

void expectEqual(std::string_view context, const ve::vulkan_renderer_detail::FrustumSphereClassification actual, const ve::vulkan_renderer_detail::FrustumSphereClassification expected) {
    if (actual != expected) {
        std::cerr << "[FAILED] " << context << ": expected " << static_cast<int>(expected) << " but got " << static_cast<int>(actual) << '\n';
        ++gFailureCount;
    }
}

void expectTrue(std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << '\n';
        ++gFailureCount;
    }
}

void expectVisible(std::string_view context, const ve::vulkan_renderer_detail::Frustum& frustum, const ve::Vec3 center, const float radius) {
    const auto classification = ve::vulkan_renderer_detail::classifySphereAgainstFrustum(frustum, center, radius);
    if (classification == ve::vulkan_renderer_detail::FrustumSphereClassification::Outside) {
        std::cerr << "[FAILED] " << context << ": expected visible sphere but got outside\n";
        ++gFailureCount;
    }
}

} // namespace

int main() {
    constexpr float kNear = 0.05f;
    constexpr float kFar = 500.0f;
    const ve::Mat4 viewProjection = ve::perspective(ve::radians(65.0f), 16.0f / 9.0f, kNear, kFar);
    const ve::vulkan_renderer_detail::Frustum frustum = ve::vulkan_renderer_detail::extractFrustumPlanes(viewProjection);

    expectVisible("reverse-Z frustum keeps near-plane sphere", frustum, {0.0f, 0.0f, -kNear}, 0.001f);
    expectVisible("reverse-Z frustum keeps mid-depth sphere", frustum, {0.0f, 0.0f, -10.0f}, 0.25f);
    expectVisible("reverse-Z frustum keeps far-plane sphere", frustum, {0.0f, 0.0f, -kFar}, 0.001f);
    expectEqual("reverse-Z frustum rejects before near plane",
                ve::vulkan_renderer_detail::classifySphereAgainstFrustum(frustum, {0.0f, 0.0f, -kNear * 0.25f}, 0.001f),
                ve::vulkan_renderer_detail::FrustumSphereClassification::Outside);
    expectEqual("reverse-Z frustum rejects beyond far plane",
                ve::vulkan_renderer_detail::classifySphereAgainstFrustum(frustum, {0.0f, 0.0f, -kFar * 1.01f}, 0.001f),
                ve::vulkan_renderer_detail::FrustumSphereClassification::Outside);

    expectTrue("texture extent accepts equal device limit",
               ve::vulkan_renderer_detail::textureExtentFitsDeviceLimit(VkExtent2D{4096U, 2048U}, 4096U));
    expectTrue("texture extent rejects width above device limit",
               !ve::vulkan_renderer_detail::textureExtentFitsDeviceLimit(VkExtent2D{4097U, 2048U}, 4096U));
    expectTrue("texture extent rejects invalid zero device limit",
               !ve::vulkan_renderer_detail::textureExtentFitsDeviceLimit(VkExtent2D{1U, 1U}, 0U));

    const VkSemaphore firstUploadSemaphore = fakeHandle<VkSemaphore>(1);
    const VkSemaphore secondUploadSemaphore = fakeHandle<VkSemaphore>(2);
    std::vector<TestPendingUpload> pendingUploads{{firstUploadSemaphore}, {VK_NULL_HANDLE}, {secondUploadSemaphore}};
    std::vector<VkSemaphore> queuedSemaphores;
    queuedSemaphores.reserve(2);
    ve::vulkan_renderer_detail::queueReservedUploadWaitSemaphores(pendingUploads, queuedSemaphores);
    expectTrue("upload queue transfers only non-null semaphores", queuedSemaphores.size() == 2U);
    expectTrue("upload queue preserves semaphore order",
               queuedSemaphores.size() == 2U && queuedSemaphores[0] == firstUploadSemaphore && queuedSemaphores[1] == secondUploadSemaphore);
    expectTrue("upload queue clears transferred semaphores",
               pendingUploads[0].signalSemaphore == VK_NULL_HANDLE && pendingUploads[1].signalSemaphore == VK_NULL_HANDLE &&
                   pendingUploads[2].signalSemaphore == VK_NULL_HANDLE);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Vulkan frustum CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
