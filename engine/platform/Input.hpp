#pragma once

#include "core/Camera.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ve {

enum class InputKey : std::uint8_t {
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Escape,
    Space,
    Enter,
    Tab,
    Backspace,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    Left,
    Right,
    Up,
    Down,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Count,
};

static_assert(static_cast<std::uint8_t>(InputKey::Count) <= 64U);

enum class InputMouseButton : std::uint8_t {
    Left,
    Right,
    Middle,
    Button4,
    Button5,
    Button6,
    Button7,
    Button8,
    Count,
};

enum class GamepadButton : std::uint8_t {
    A,
    B,
    X,
    Y,
    LeftBumper,
    RightBumper,
    Back,
    Start,
    Guide,
    LeftThumb,
    RightThumb,
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,
    Count,
};

static_assert(static_cast<std::uint8_t>(GamepadButton::Count) <= 16U);

enum class GamepadAxis : std::uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

inline constexpr std::size_t GamepadSlotCount = 4U;

struct GamepadDeadzone {
    float stick = 0.15F;
    float trigger = 0.05F;
};

struct GamepadSample {
    bool connected = false;
    std::uint16_t buttons = 0U;
    std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> axes{};
};

class GamepadState {
  public:
    [[nodiscard]] constexpr bool connected() const noexcept { return connected_; }

    [[nodiscard]] constexpr bool held(const GamepadButton button) const noexcept {
        return (heldButtons_ & buttonMask(button)) != 0U;
    }

    [[nodiscard]] constexpr bool pressed(const GamepadButton button) const noexcept {
        return (pressedButtons_ & buttonMask(button)) != 0U;
    }

    [[nodiscard]] constexpr bool released(const GamepadButton button) const noexcept {
        return (releasedButtons_ & buttonMask(button)) != 0U;
    }

    [[nodiscard]] constexpr float axis(const GamepadAxis axis) const noexcept {
        const auto ordinal = static_cast<std::uint8_t>(axis);
        return ordinal < static_cast<std::uint8_t>(GamepadAxis::Count) ? axes_[ordinal] : 0.0F;
    }

  private:
    friend class InputState;
    friend class InputTracker;

    [[nodiscard]] static constexpr std::uint16_t buttonMask(const GamepadButton button) noexcept {
        const auto ordinal = static_cast<std::uint8_t>(button);
        return ordinal < static_cast<std::uint8_t>(GamepadButton::Count)
                   ? static_cast<std::uint16_t>(std::uint16_t{1} << ordinal)
                   : 0U;
    }

    bool connected_ = false;
    std::uint16_t heldButtons_ = 0U;
    std::uint16_t pressedButtons_ = 0U;
    std::uint16_t releasedButtons_ = 0U;
    std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> axes_{};
};

class InputState {
  public:
    [[nodiscard]] constexpr bool held(const InputKey key) const noexcept { return (heldKeys_ & keyMask(key)) != 0U; }

    [[nodiscard]] constexpr bool pressed(const InputKey key) const noexcept {
        return (pressedKeys_ & keyMask(key)) != 0U;
    }

    [[nodiscard]] constexpr bool released(const InputKey key) const noexcept {
        return (releasedKeys_ & keyMask(key)) != 0U;
    }

    [[nodiscard]] constexpr bool held(const InputMouseButton button) const noexcept {
        return (heldMouseButtons_ & mouseButtonMask(button)) != 0U;
    }

    [[nodiscard]] constexpr bool pressed(const InputMouseButton button) const noexcept {
        return (pressedMouseButtons_ & mouseButtonMask(button)) != 0U;
    }

    [[nodiscard]] constexpr bool released(const InputMouseButton button) const noexcept {
        return (releasedMouseButtons_ & mouseButtonMask(button)) != 0U;
    }

    double cursorX = 0.0;
    double cursorY = 0.0;
    double cursorDeltaX = 0.0;
    double cursorDeltaY = 0.0;
    double scrollDeltaX = 0.0;
    double scrollDeltaY = 0.0;
    bool cursorCaptured = false;

    [[nodiscard]] constexpr const GamepadState &gamepad(const std::size_t slot) const noexcept {
        return slot < GamepadSlotCount ? gamepads_[slot] : disconnectedGamepad_;
    }

  private:
    friend class InputTracker;

    [[nodiscard]] static constexpr std::uint64_t keyMask(const InputKey key) noexcept {
        const auto ordinal = static_cast<std::uint8_t>(key);
        return ordinal < static_cast<std::uint8_t>(InputKey::Count) ? std::uint64_t{1} << ordinal : 0U;
    }

    [[nodiscard]] static constexpr std::uint8_t mouseButtonMask(const InputMouseButton button) noexcept {
        const auto ordinal = static_cast<std::uint8_t>(button);
        return ordinal < static_cast<std::uint8_t>(InputMouseButton::Count)
                   ? static_cast<std::uint8_t>(std::uint8_t{1} << ordinal)
                   : 0U;
    }

    std::uint64_t heldKeys_ = 0U;
    std::uint64_t pressedKeys_ = 0U;
    std::uint64_t releasedKeys_ = 0U;
    std::uint8_t heldMouseButtons_ = 0U;
    std::uint8_t pressedMouseButtons_ = 0U;
    std::uint8_t releasedMouseButtons_ = 0U;
    std::array<GamepadState, GamepadSlotCount> gamepads_{};
    inline static constexpr GamepadState disconnectedGamepad_{};
};

class InputTracker {
  public:
    void keyEvent(InputKey key, bool down) noexcept;
    void mouseButtonEvent(InputMouseButton button, bool down) noexcept;
    void cursorPosition(double x, double y) noexcept;
    void scrollEvent(double xOffset, double yOffset) noexcept;
    void accumulate(const InputState &state) noexcept;
    void gamepadSample(std::size_t slot, const GamepadSample &sample, GamepadDeadzone deadzone = {}) noexcept;

    void beginCapture() noexcept;
    void endCapture() noexcept;
    void focusLost() noexcept;

    [[nodiscard]] InputState consume() noexcept;

  private:
    std::uint64_t heldKeys_ = 0U;
    std::uint64_t pressedKeys_ = 0U;
    std::uint64_t releasedKeys_ = 0U;
    std::uint8_t heldMouseButtons_ = 0U;
    std::uint8_t pressedMouseButtons_ = 0U;
    std::uint8_t releasedMouseButtons_ = 0U;
    double cursorX_ = 0.0;
    double cursorY_ = 0.0;
    double cursorDeltaX_ = 0.0;
    double cursorDeltaY_ = 0.0;
    double scrollDeltaX_ = 0.0;
    double scrollDeltaY_ = 0.0;
    bool hasCursorPosition_ = false;
    bool cursorCaptured_ = false;
    std::array<GamepadState, GamepadSlotCount> gamepads_{};
};

struct CameraInput {
    float forward = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float mouseYawDegrees = 0.0f;
    float mousePitchDegrees = 0.0f;
};

[[nodiscard]] CameraInput mapCameraInput(bool forwardKey, bool backKey, bool rightKey, bool leftKey, bool upKey,
                                         bool downKey, bool lookRight, bool lookLeft, bool lookUp,
                                         bool lookDown) noexcept;

[[nodiscard]] CameraInput mapCameraInput(const InputState &state, float mouseSensitivityDegrees = 0.12f) noexcept;

void applyCameraInput(Camera &camera, const CameraInput &input, float dt);

} // namespace ve
