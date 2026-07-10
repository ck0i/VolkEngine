#pragma once

#include "core/Camera.hpp"
#include "core/Config.hpp"
#include "core/Time.hpp"
#include "core/World.hpp"
#include "platform/Window.hpp"
#include "renderer/SceneRenderer.hpp"
#include "renderer/vulkan/VulkanRenderer.hpp"

namespace ve {

class Application {
public:
    explicit Application(EngineConfig config);
    using WorldUpdateCallback = void (*)(World&, double simulationElapsedSeconds, double simulationDeltaSeconds);
    int run(const RunOptions& options);
    int run(World& world, const RunOptions& options);
    int run(World& world, WorldUpdateCallback update, const RunOptions& options);

private:
    int runInternal(World* world, WorldUpdateCallback update, const RunOptions& options);
    EngineConfig config_;
    GlfwRuntime glfwRuntime_;
    Window window_;
    Camera camera_;
    VulkanRenderer renderer_;
    DemoSceneRenderer sceneRenderer_;
    WorldSceneExtractor worldSceneExtractor_;
    Clock clock_;
    double simulationElapsedSeconds_ = 0.0;
};

} // namespace ve
