#pragma once

#include "core/Config.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/Renderer.hpp"

#include <filesystem>
#include <memory>

namespace ve {
class Window;

class VulkanRenderer final : public IRenderer {
public:
    VulkanRenderer(Window& window, EngineConfig config);
    ~VulkanRenderer() override;

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    VulkanRenderer(VulkanRenderer&&) = delete;
    VulkanRenderer& operator=(VulkanRenderer&&) = delete;

    void draw(const Camera& camera, const SceneRenderList& scene, double sceneBuildMs,
              double elapsedSeconds, double frameDeltaMs) override;
    [[nodiscard]] MeshBounds meshBounds(SceneMeshId mesh) const;
    [[nodiscard]] RenderStats stats() const override;
    [[nodiscard]] const RenderDeviceInfo& deviceInfo() const override;
    void requestScreenshot(std::filesystem::path path);
    void waitIdle();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ve
