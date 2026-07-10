#pragma once

#include "core/Camera.hpp"
#include "core/Config.hpp"
#include "core/Time.hpp"
#include "platform/Window.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/vulkan/VulkanRenderer.hpp"

namespace ve {

class Application {
public:
    explicit Application(EngineConfig config);
    int run(const RunOptions& options);

private:
    EngineConfig config_;
    Window window_;
    Camera camera_;
    VulkanRenderer renderer_;
    DemoSceneRenderer sceneRenderer_;
    Clock clock_;
    double simulationElapsedSeconds_ = 0.0;
};

} // namespace ve
