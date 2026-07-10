#pragma once

#include "core/Config.hpp"
#include "core/Camera.hpp"

#include <cstdint>
#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace ve {

class GlfwRuntime final {
public:
    GlfwRuntime();
    ~GlfwRuntime();

    GlfwRuntime(const GlfwRuntime&) = delete;
    GlfwRuntime& operator=(const GlfwRuntime&) = delete;
    GlfwRuntime(GlfwRuntime&&) = delete;
    GlfwRuntime& operator=(GlfwRuntime&&) = delete;
};

class Window {
public:
    explicit Window(GlfwRuntime& runtime, const EngineConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    void pollEvents();
    void waitEvents();
    [[nodiscard]] bool shouldClose() const;
    void requestClose();
    void updateCamera(Camera& camera, float dt);
    void setSize(std::uint32_t width, std::uint32_t height);
    void setTitle(const char* title);

    [[nodiscard]] VkExtent2D framebufferExtent() const;
    [[nodiscard]] bool consumeFramebufferResized();
    [[nodiscard]] GLFWwindow* handle() const { return window_; }

    void createSurface(VkInstance instance, VkSurfaceKHR* surface) const;

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* window_ = nullptr;
    mutable bool framebufferResized_ = false;
    bool mouseLookActive_ = false;
    double lastCursorX_ = 0.0;
    double lastCursorY_ = 0.0;
};

} // namespace ve
