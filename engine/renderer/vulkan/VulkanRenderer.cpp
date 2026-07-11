#include "renderer/vulkan/VulkanRendererImpl.hpp"

#include <utility>

namespace ve {

VulkanRenderer::VulkanRenderer(Window &window, EngineConfig config,
                               ReferenceAssetBundle &referenceAssets)
    : impl_(
          std::make_unique<Impl>(window, std::move(config), referenceAssets)) {}

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::draw(const Camera &camera, const SceneRenderList &scene,
                          const double sceneBuildMs,
                          const double elapsedSeconds,
                          const double frameDeltaMs) {
  impl_->draw(camera, scene, sceneBuildMs, elapsedSeconds, frameDeltaMs);
}

MeshBounds VulkanRenderer::meshBounds(const MeshAssetHandle mesh) const {
  return impl_->meshBounds(mesh);
}

std::array<TextureAssetHandle, 3>
VulkanRenderer::materialTextureHandles(const AssetId material) const {
  return impl_->materialTextureHandles(material);
}

RenderStats VulkanRenderer::stats() const { return impl_->stats(); }

void VulkanRenderer::reloadReferenceAssets(ReferenceAssetBundle candidate) {
  impl_->reloadReferenceAssets(std::move(candidate));
}

const RenderDeviceInfo &VulkanRenderer::deviceInfo() const {
    return impl_->deviceInfo();
}

void VulkanRenderer::requestScreenshot(std::filesystem::path path) {
    impl_->requestScreenshot(std::move(path));
}

void VulkanRenderer::armAcquireRecoverySmoke() {
  impl_->armAcquireRecoverySmoke();
}

void VulkanRenderer::waitIdle() { impl_->waitIdle(); }

} // namespace ve
