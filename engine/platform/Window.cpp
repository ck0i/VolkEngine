#include "platform/Window.hpp"
#include "platform/Input.hpp"

#include "core/Log.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <stdexcept>
#include <limits>

namespace ve {
namespace {
int g_windowCount = 0;

void validateWindowDimensions(const std::uint32_t width, const std::uint32_t height) {
    constexpr std::uint32_t kMaxGlfwWindowExtent = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (width == 0U || height == 0U || width > kMaxGlfwWindowExtent || height > kMaxGlfwWindowExtent) {
        throw std::runtime_error("Window dimensions must be positive and fit GLFW's int extent range");
    }
}
}

Window::Window(const EngineConfig& config) {
    validateWindowDimensions(config.initialWidth, config.initialHeight);
    const bool ownsGlfwInit = g_windowCount == 0;
    if (ownsGlfwInit) {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("Failed to initialize GLFW");
        }
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(static_cast<int>(config.initialWidth), static_cast<int>(config.initialHeight), config.applicationName.c_str(), nullptr, nullptr);
    if (!window_) {
        if (ownsGlfwInit) {
            glfwTerminate();
        }
        throw std::runtime_error("Failed to create GLFW window");
    }
    ++g_windowCount;

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    logger()->info("Created window {}x{}", config.initialWidth, config.initialHeight);
}

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    --g_windowCount;
    if (g_windowCount == 0) {
        glfwTerminate();
    }
}

void Window::pollEvents() {
    glfwPollEvents();
}

void Window::waitEvents() {
    glfwWaitEvents();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void Window::requestClose() {
    glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void Window::updateCamera(Camera& camera, const float dt) {
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    CameraInput input = mapCameraInput(
        glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_RIGHT) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_LEFT) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_UP) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_DOWN) == GLFW_PRESS);

    if (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        double cursorX = 0.0;
        double cursorY = 0.0;
        glfwGetCursorPos(window_, &cursorX, &cursorY);
        if (!mouseLookActive_) {
            mouseLookActive_ = true;
            lastCursorX_ = cursorX;
            lastCursorY_ = cursorY;
            glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else {
            constexpr float mouseSensitivity = 0.12f;
            input.mouseYawDegrees = static_cast<float>(cursorX - lastCursorX_) * mouseSensitivity;
            input.mousePitchDegrees = static_cast<float>(lastCursorY_ - cursorY) * mouseSensitivity;
            lastCursorX_ = cursorX;
            lastCursorY_ = cursorY;
        }
    } else if (mouseLookActive_) {
        mouseLookActive_ = false;
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    applyCameraInput(camera, input, dt);
}

void Window::setSize(const std::uint32_t width, const std::uint32_t height) {
    validateWindowDimensions(width, height);
    glfwSetWindowSize(window_, static_cast<int>(width), static_cast<int>(height));
}

void Window::setTitle(const char* title) {
    glfwSetWindowTitle(window_, title);
}

VkExtent2D Window::framebufferExtent() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

bool Window::consumeFramebufferResized() {
    const bool resized = framebufferResized_;
    framebufferResized_ = false;
    return resized;
}

void Window::createSurface(VkInstance instance, VkSurfaceKHR* surface) const {
    if (glfwCreateWindowSurface(instance, window_, nullptr, surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan window surface");
    }
}

void Window::framebufferResizeCallback(GLFWwindow* window, int, int) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->framebufferResized_ = true;
    }
}

} // namespace ve
