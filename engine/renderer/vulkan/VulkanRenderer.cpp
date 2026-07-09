#include "renderer/vulkan/VulkanRendererImpl.hpp"

#include <utility>

namespace ve {

VulkanRenderer::VulkanRenderer(Window& window, EngineConfig config)
    : impl_(std::make_unique<Impl>(window, std::move(config))) {}

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::draw(const Camera& camera, const double elapsedSeconds, const double frameDeltaMs) {
    impl_->draw(camera, elapsedSeconds, frameDeltaMs);
}

RenderStats VulkanRenderer::stats() const {
    return impl_->stats();
}

const RenderDeviceInfo& VulkanRenderer::deviceInfo() const {
    return impl_->deviceInfo();
}

void VulkanRenderer::requestScreenshot(std::filesystem::path path) {
    impl_->requestScreenshot(std::move(path));
}

void VulkanRenderer::waitIdle() {
    impl_->waitIdle();
}

} // namespace ve
