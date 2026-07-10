#pragma once

#include "core/Config.hpp"
#include "core/Camera.hpp"
#include "platform/Input.hpp"

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
    [[nodiscard]] InputState pollInput();
    void updateCamera(Camera& camera, const InputState& input, float dt);
    void updateCamera(Camera& camera, float dt);
    void setSize(std::uint32_t width, std::uint32_t height);
    void setTitle(const char* title);

    [[nodiscard]] VkExtent2D framebufferExtent() const;
    [[nodiscard]] bool consumeFramebufferResized();
    [[nodiscard]] GLFWwindow* handle() const { return window_; }

    void createSurface(VkInstance instance, VkSurfaceKHR* surface) const;

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void cursorPositionCallback(GLFWwindow* window, double x, double y);
    static void focusCallback(GLFWwindow* window, int focused);

    void beginCursorCapture();
    void endCursorCapture();

    GLFWwindow* window_ = nullptr;
    mutable bool framebufferResized_ = false;
    InputTracker inputTracker_;
};

} // namespace ve
