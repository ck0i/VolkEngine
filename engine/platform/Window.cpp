#include "platform/Window.hpp"
#include "platform/Input.hpp"

#include "core/Log.hpp"

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <limits>
#include <stdexcept>

namespace ve {
namespace {

bool g_glfwRuntimeActive = false;

void validateWindowDimensions(const std::uint32_t width, const std::uint32_t height) {
    constexpr std::uint32_t kMaxGlfwWindowExtent = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (width == 0U || height == 0U || width > kMaxGlfwWindowExtent || height > kMaxGlfwWindowExtent) {
        throw std::runtime_error("Window dimensions must be positive and fit GLFW's int extent range");
    }
}

bool mapGlfwKey(const int glfwKey, InputKey &inputKey) noexcept {
    switch (glfwKey) {
    case GLFW_KEY_A: inputKey = InputKey::A; return true;
    case GLFW_KEY_B: inputKey = InputKey::B; return true;
    case GLFW_KEY_C: inputKey = InputKey::C; return true;
    case GLFW_KEY_D: inputKey = InputKey::D; return true;
    case GLFW_KEY_E: inputKey = InputKey::E; return true;
    case GLFW_KEY_F: inputKey = InputKey::F; return true;
    case GLFW_KEY_G: inputKey = InputKey::G; return true;
    case GLFW_KEY_H: inputKey = InputKey::H; return true;
    case GLFW_KEY_I: inputKey = InputKey::I; return true;
    case GLFW_KEY_J: inputKey = InputKey::J; return true;
    case GLFW_KEY_K: inputKey = InputKey::K; return true;
    case GLFW_KEY_L: inputKey = InputKey::L; return true;
    case GLFW_KEY_M: inputKey = InputKey::M; return true;
    case GLFW_KEY_N: inputKey = InputKey::N; return true;
    case GLFW_KEY_O: inputKey = InputKey::O; return true;
    case GLFW_KEY_P: inputKey = InputKey::P; return true;
    case GLFW_KEY_Q: inputKey = InputKey::Q; return true;
    case GLFW_KEY_R: inputKey = InputKey::R; return true;
    case GLFW_KEY_S: inputKey = InputKey::S; return true;
    case GLFW_KEY_T: inputKey = InputKey::T; return true;
    case GLFW_KEY_U: inputKey = InputKey::U; return true;
    case GLFW_KEY_V: inputKey = InputKey::V; return true;
    case GLFW_KEY_W: inputKey = InputKey::W; return true;
    case GLFW_KEY_X: inputKey = InputKey::X; return true;
    case GLFW_KEY_Y: inputKey = InputKey::Y; return true;
    case GLFW_KEY_Z: inputKey = InputKey::Z; return true;
    case GLFW_KEY_0: inputKey = InputKey::Digit0; return true;
    case GLFW_KEY_1: inputKey = InputKey::Digit1; return true;
    case GLFW_KEY_2: inputKey = InputKey::Digit2; return true;
    case GLFW_KEY_3: inputKey = InputKey::Digit3; return true;
    case GLFW_KEY_4: inputKey = InputKey::Digit4; return true;
    case GLFW_KEY_5: inputKey = InputKey::Digit5; return true;
    case GLFW_KEY_6: inputKey = InputKey::Digit6; return true;
    case GLFW_KEY_7: inputKey = InputKey::Digit7; return true;
    case GLFW_KEY_8: inputKey = InputKey::Digit8; return true;
    case GLFW_KEY_9: inputKey = InputKey::Digit9; return true;
    case GLFW_KEY_ESCAPE: inputKey = InputKey::Escape; return true;
    case GLFW_KEY_SPACE: inputKey = InputKey::Space; return true;
    case GLFW_KEY_ENTER: inputKey = InputKey::Enter; return true;
    case GLFW_KEY_TAB: inputKey = InputKey::Tab; return true;
    case GLFW_KEY_BACKSPACE: inputKey = InputKey::Backspace; return true;
    case GLFW_KEY_LEFT_SHIFT: inputKey = InputKey::LeftShift; return true;
    case GLFW_KEY_RIGHT_SHIFT: inputKey = InputKey::RightShift; return true;
    case GLFW_KEY_LEFT_CONTROL: inputKey = InputKey::LeftControl; return true;
    case GLFW_KEY_RIGHT_CONTROL: inputKey = InputKey::RightControl; return true;
    case GLFW_KEY_LEFT_ALT: inputKey = InputKey::LeftAlt; return true;
    case GLFW_KEY_RIGHT_ALT: inputKey = InputKey::RightAlt; return true;
    case GLFW_KEY_LEFT: inputKey = InputKey::Left; return true;
    case GLFW_KEY_RIGHT: inputKey = InputKey::Right; return true;
    case GLFW_KEY_UP: inputKey = InputKey::Up; return true;
    case GLFW_KEY_DOWN: inputKey = InputKey::Down; return true;
    case GLFW_KEY_F1: inputKey = InputKey::F1; return true;
    case GLFW_KEY_F2: inputKey = InputKey::F2; return true;
    case GLFW_KEY_F3: inputKey = InputKey::F3; return true;
    case GLFW_KEY_F4: inputKey = InputKey::F4; return true;
    case GLFW_KEY_F5: inputKey = InputKey::F5; return true;
    case GLFW_KEY_F6: inputKey = InputKey::F6; return true;
    case GLFW_KEY_F7: inputKey = InputKey::F7; return true;
    case GLFW_KEY_F8: inputKey = InputKey::F8; return true;
    case GLFW_KEY_F9: inputKey = InputKey::F9; return true;
    case GLFW_KEY_F10: inputKey = InputKey::F10; return true;
    case GLFW_KEY_F11: inputKey = InputKey::F11; return true;
    case GLFW_KEY_F12: inputKey = InputKey::F12; return true;
    default: return false;
    }
}

bool mapGlfwMouseButton(const int glfwButton, InputMouseButton &inputButton) noexcept {
    switch (glfwButton) {
    case GLFW_MOUSE_BUTTON_LEFT: inputButton = InputMouseButton::Left; return true;
    case GLFW_MOUSE_BUTTON_RIGHT: inputButton = InputMouseButton::Right; return true;
    case GLFW_MOUSE_BUTTON_MIDDLE: inputButton = InputMouseButton::Middle; return true;
    case GLFW_MOUSE_BUTTON_4: inputButton = InputMouseButton::Button4; return true;
    case GLFW_MOUSE_BUTTON_5: inputButton = InputMouseButton::Button5; return true;
    case GLFW_MOUSE_BUTTON_6: inputButton = InputMouseButton::Button6; return true;
    case GLFW_MOUSE_BUTTON_7: inputButton = InputMouseButton::Button7; return true;
    case GLFW_MOUSE_BUTTON_8: inputButton = InputMouseButton::Button8; return true;
    default: return false;
    }
}
[[nodiscard]] constexpr std::uint16_t gamepadButtonMask(const GamepadButton button) noexcept {
    return static_cast<std::uint16_t>(std::uint16_t{1} << static_cast<std::uint8_t>(button));
}

void mapGlfwGamepadButtons(const GLFWgamepadstate &glfwState, GamepadSample &sample) noexcept {
    const auto mapButton = [&glfwState, &sample](const int glfwButton, const GamepadButton button) {
        if (glfwState.buttons[glfwButton] == GLFW_PRESS) {
            sample.buttons |= gamepadButtonMask(button);
        }
    };

    mapButton(GLFW_GAMEPAD_BUTTON_A, GamepadButton::A);
    mapButton(GLFW_GAMEPAD_BUTTON_B, GamepadButton::B);
    mapButton(GLFW_GAMEPAD_BUTTON_X, GamepadButton::X);
    mapButton(GLFW_GAMEPAD_BUTTON_Y, GamepadButton::Y);
    mapButton(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, GamepadButton::LeftBumper);
    mapButton(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, GamepadButton::RightBumper);
    mapButton(GLFW_GAMEPAD_BUTTON_BACK, GamepadButton::Back);
    mapButton(GLFW_GAMEPAD_BUTTON_START, GamepadButton::Start);
    mapButton(GLFW_GAMEPAD_BUTTON_GUIDE, GamepadButton::Guide);
    mapButton(GLFW_GAMEPAD_BUTTON_LEFT_THUMB, GamepadButton::LeftThumb);
    mapButton(GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, GamepadButton::RightThumb);
    mapButton(GLFW_GAMEPAD_BUTTON_DPAD_UP, GamepadButton::DpadUp);
    mapButton(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, GamepadButton::DpadRight);
    mapButton(GLFW_GAMEPAD_BUTTON_DPAD_DOWN, GamepadButton::DpadDown);
    mapButton(GLFW_GAMEPAD_BUTTON_DPAD_LEFT, GamepadButton::DpadLeft);
}

void mapGlfwGamepadAxes(const GLFWgamepadstate &glfwState, GamepadSample &sample) noexcept {
    sample.axes[static_cast<std::size_t>(GamepadAxis::LeftX)] = glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
    sample.axes[static_cast<std::size_t>(GamepadAxis::LeftY)] = -glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
    sample.axes[static_cast<std::size_t>(GamepadAxis::RightX)] = glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
    sample.axes[static_cast<std::size_t>(GamepadAxis::RightY)] = -glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
    sample.axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] =
        (glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0F) * 0.5F;
    sample.axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] =
        (glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0F) * 0.5F;
}

void pollGlfwGamepad(InputTracker &tracker, const std::size_t slot) noexcept {
    GamepadSample sample{};
    GLFWgamepadstate glfwState{};
    if (glfwGetGamepadState(GLFW_JOYSTICK_1 + static_cast<int>(slot), &glfwState) == GLFW_TRUE) {
        sample.connected = true;
        mapGlfwGamepadButtons(glfwState, sample);
        mapGlfwGamepadAxes(glfwState, sample);
    }
    tracker.gamepadSample(slot, sample);
}

} // namespace

GlfwRuntime::GlfwRuntime() {
    if (g_glfwRuntimeActive) {
        throw std::runtime_error("Only one GlfwRuntime may be active per process");
    }
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("Failed to initialize GLFW");
    }
    g_glfwRuntimeActive = true;
}

GlfwRuntime::~GlfwRuntime() {
    g_glfwRuntimeActive = false;
    glfwTerminate();
}

Window::Window(GlfwRuntime &, const EngineConfig &config) {
    validateWindowDimensions(config.initialWidth, config.initialHeight);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(static_cast<int>(config.initialWidth), static_cast<int>(config.initialHeight),
                               config.applicationName.c_str(), nullptr, nullptr);
    if (!window_) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetKeyCallback(window_, keyCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwSetCursorPosCallback(window_, cursorPositionCallback);
    glfwSetScrollCallback(window_, scrollCallback);
    glfwSetWindowFocusCallback(window_, focusCallback);
    logger()->info("Created window {}x{}", config.initialWidth, config.initialHeight);
}

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
}

void Window::pollEvents() { glfwPollEvents(); }

void Window::waitEvents() { glfwWaitEvents(); }

bool Window::shouldClose() const { return glfwWindowShouldClose(window_) == GLFW_TRUE; }

void Window::requestClose() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }

InputState Window::pollInput() {
    for (std::size_t slot = 0; slot < GamepadSlotCount; ++slot) {
        pollGlfwGamepad(inputTracker_, slot);
    }
    return inputTracker_.consume();
}

void Window::updateCamera(Camera &camera, const InputState &state, const float dt) {
    const CameraInput input = mapCameraInput(state);
    applyCameraInput(camera, input, dt);
}

void Window::updateCamera(Camera &camera, const float dt) { updateCamera(camera, pollInput(), dt); }

void Window::setSize(const std::uint32_t width, const std::uint32_t height) {
    validateWindowDimensions(width, height);
    glfwSetWindowSize(window_, static_cast<int>(width), static_cast<int>(height));
}

void Window::setTitle(const char *title) { glfwSetWindowTitle(window_, title); }

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

void Window::createSurface(VkInstance instance, VkSurfaceKHR *surface) const {
    if (glfwCreateWindowSurface(instance, window_, nullptr, surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan window surface");
    }
}

void Window::framebufferResizeCallback(GLFWwindow *window, int, int) {
    auto *self = static_cast<Window *>(glfwGetWindowUserPointer(window));
    if (self) {
        self->framebufferResized_ = true;
    }
}

void Window::keyCallback(GLFWwindow *window, const int key, int, const int action, int) {
    auto *self = static_cast<Window *>(glfwGetWindowUserPointer(window));
    if (!self || (action != GLFW_PRESS && action != GLFW_RELEASE)) {
        return;
    }

    InputKey inputKey{};
    if (!mapGlfwKey(key, inputKey)) {
        return;
    }

    self->inputTracker_.keyEvent(inputKey, action == GLFW_PRESS);
    if (inputKey == InputKey::Escape && action == GLFW_PRESS) {
        self->requestClose();
    }
}

void Window::mouseButtonCallback(GLFWwindow *window, const int button, const int action, int) {
    auto *self = static_cast<Window *>(glfwGetWindowUserPointer(window));
    if (!self || (action != GLFW_PRESS && action != GLFW_RELEASE)) {
        return;
    }

    InputMouseButton inputButton{};
    if (!mapGlfwMouseButton(button, inputButton)) {
        return;
    }

    self->inputTracker_.mouseButtonEvent(inputButton, action == GLFW_PRESS);
    if (inputButton == InputMouseButton::Right) {
        if (action == GLFW_PRESS) {
            self->beginCursorCapture();
        } else {
            self->inputTracker_.endCapture();
            self->endCursorCapture();
        }
    }
}

void Window::cursorPositionCallback(GLFWwindow *window, const double x, const double y) {
    auto *self = static_cast<Window *>(glfwGetWindowUserPointer(window));
    if (self) {
        self->inputTracker_.cursorPosition(x, y);
    }
}

void Window::scrollCallback(GLFWwindow *window, const double xOffset, const double yOffset) {
    auto *self = static_cast<Window *>(glfwGetWindowUserPointer(window));
    if (self) {
        self->inputTracker_.scrollEvent(xOffset, yOffset);
    }
}

void Window::focusCallback(GLFWwindow *window, const int focused) {
    auto *self = static_cast<Window *>(glfwGetWindowUserPointer(window));
    if (self && focused == GLFW_FALSE) {
        self->inputTracker_.focusLost();
        self->endCursorCapture();
    }
}

void Window::beginCursorCapture() {
    inputTracker_.beginCapture();
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
}

void Window::endCursorCapture() {
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

} // namespace ve
