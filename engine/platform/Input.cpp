#include "platform/Input.hpp"

#include <algorithm>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

[[nodiscard]] float clampFinite(const float value, const float minimum, const float maximum) noexcept {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : 0.0F;
}

[[nodiscard]] float clampDeadzone(const float value) noexcept { return clampFinite(value, 0.0F, 1.0F); }

void normalizeStick(float &x, float &y, const float deadzone) noexcept {
    x = clampFinite(x, -1.0F, 1.0F);
    y = clampFinite(y, -1.0F, 1.0F);
    const float length = std::sqrt(x * x + y * y);
    const float threshold = clampDeadzone(deadzone);
    const float cappedLength = std::min(length, 1.0F);
    if (length == 0.0F || cappedLength <= threshold || threshold >= 1.0F) {
        x = 0.0F;
        y = 0.0F;
        return;
    }

    const float scale = (cappedLength - threshold) / ((1.0F - threshold) * length);
    x *= scale;
    y *= scale;
}

[[nodiscard]] float normalizeTrigger(const float value, const float deadzone) noexcept {
    const float threshold = clampDeadzone(deadzone);
    const float clamped = clampFinite(value, 0.0F, 1.0F);
    return clamped <= threshold || threshold >= 1.0F ? 0.0F : (clamped - threshold) / (1.0F - threshold);
}

} // namespace

namespace ve {

void InputTracker::keyEvent(const InputKey key, const bool down) noexcept {
    const std::uint64_t mask = InputState::keyMask(key);
    if (down) {
        if ((heldKeys_ & mask) == 0U) {
            heldKeys_ |= mask;
            pressedKeys_ |= mask;
        }
        return;
    }

    if ((heldKeys_ & mask) != 0U) {
        heldKeys_ &= ~mask;
        releasedKeys_ |= mask;
    }
}

void InputTracker::mouseButtonEvent(const InputMouseButton button, const bool down) noexcept {
    const std::uint8_t mask = InputState::mouseButtonMask(button);
    if (down) {
        if ((heldMouseButtons_ & mask) == 0U) {
            heldMouseButtons_ |= mask;
            pressedMouseButtons_ |= mask;
        }
        return;
    }

    if ((heldMouseButtons_ & mask) != 0U) {
        heldMouseButtons_ = static_cast<std::uint8_t>(heldMouseButtons_ & ~mask);
        releasedMouseButtons_ |= mask;
    }
}

void InputTracker::cursorPosition(const double x, const double y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    if (hasCursorPosition_) {
        const double deltaX = x - cursorX_;
        const double deltaY = y - cursorY_;
        const double accumulatedX = cursorDeltaX_ + deltaX;
        const double accumulatedY = cursorDeltaY_ + deltaY;
        if (std::isfinite(deltaX) && std::isfinite(deltaY) && std::isfinite(accumulatedX) &&
            std::isfinite(accumulatedY)) {
            cursorDeltaX_ = accumulatedX;
            cursorDeltaY_ = accumulatedY;
        } else {
            cursorDeltaX_ = 0.0;
            cursorDeltaY_ = 0.0;
        }
    }
    cursorX_ = x;
    cursorY_ = y;
    hasCursorPosition_ = true;
}

void InputTracker::scrollEvent(const double xOffset, const double yOffset) noexcept {
    if (!std::isfinite(xOffset) || !std::isfinite(yOffset)) {
        return;
    }
    const double accumulatedX = scrollDeltaX_ + xOffset;
    const double accumulatedY = scrollDeltaY_ + yOffset;
    if (std::isfinite(accumulatedX) && std::isfinite(accumulatedY)) {
        scrollDeltaX_ = accumulatedX;
        scrollDeltaY_ = accumulatedY;
    } else {
        scrollDeltaX_ = 0.0;

        scrollDeltaY_ = 0.0;
    }
}

void InputTracker::gamepadSample(const std::size_t slot, const GamepadSample &sample,
                                 const GamepadDeadzone deadzone) noexcept {
    if (slot >= GamepadSlotCount) {
        return;
    }

    GamepadState &state = gamepads_[slot];
    if (!sample.connected) {
        state.releasedButtons_ |= state.heldButtons_;
        state.heldButtons_ = 0U;
        state.connected_ = false;
        state.axes_.fill(0.0F);
        return;
    }

    constexpr std::uint16_t buttonMask =
        static_cast<std::uint16_t>((std::uint16_t{1} << static_cast<std::uint8_t>(GamepadButton::Count)) - 1U);
    const std::uint16_t heldButtons = static_cast<std::uint16_t>(sample.buttons & buttonMask);
    state.pressedButtons_ |= static_cast<std::uint16_t>(heldButtons & ~state.heldButtons_);
    state.releasedButtons_ |= static_cast<std::uint16_t>(state.heldButtons_ & ~heldButtons);
    state.heldButtons_ = heldButtons;
    state.connected_ = true;

    state.axes_ = sample.axes;
    normalizeStick(state.axes_[static_cast<std::size_t>(GamepadAxis::LeftX)],
                   state.axes_[static_cast<std::size_t>(GamepadAxis::LeftY)], deadzone.stick);
    normalizeStick(state.axes_[static_cast<std::size_t>(GamepadAxis::RightX)],
                   state.axes_[static_cast<std::size_t>(GamepadAxis::RightY)], deadzone.stick);
    state.axes_[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] =
        normalizeTrigger(state.axes_[static_cast<std::size_t>(GamepadAxis::LeftTrigger)], deadzone.trigger);
    state.axes_[static_cast<std::size_t>(GamepadAxis::RightTrigger)] =
        normalizeTrigger(state.axes_[static_cast<std::size_t>(GamepadAxis::RightTrigger)], deadzone.trigger);
}

void InputTracker::accumulate(const InputState &state) noexcept {
    heldKeys_ = state.heldKeys_;
    pressedKeys_ |= state.pressedKeys_;
    releasedKeys_ |= state.releasedKeys_;
    heldMouseButtons_ = state.heldMouseButtons_;
    pressedMouseButtons_ |= state.pressedMouseButtons_;
    releasedMouseButtons_ |= state.releasedMouseButtons_;

    for (std::size_t slot = 0U; slot < GamepadSlotCount; ++slot) {
        const GamepadState &source = state.gamepads_[slot];
        GamepadState &target = gamepads_[slot];
        target.connected_ = source.connected_;
        target.heldButtons_ = source.heldButtons_;
        target.pressedButtons_ |= source.pressedButtons_;
        target.releasedButtons_ |= source.releasedButtons_;
        target.axes_ = source.axes_;
        for (std::size_t axis = 0U; axis < target.axes_.size(); ++axis) {
            const bool trigger = axis == static_cast<std::size_t>(GamepadAxis::LeftTrigger) ||
                                 axis == static_cast<std::size_t>(GamepadAxis::RightTrigger);
            target.axes_[axis] = clampFinite(target.axes_[axis], trigger ? 0.0F : -1.0F, 1.0F);
        }
    }

    if (std::isfinite(state.cursorX) && std::isfinite(state.cursorY)) {
        cursorX_ = state.cursorX;
        cursorY_ = state.cursorY;
        hasCursorPosition_ = true;
    }

    const double accumulatedCursorX = cursorDeltaX_ + state.cursorDeltaX;
    const double accumulatedCursorY = cursorDeltaY_ + state.cursorDeltaY;
    if (std::isfinite(accumulatedCursorX) && std::isfinite(accumulatedCursorY)) {
        cursorDeltaX_ = accumulatedCursorX;
        cursorDeltaY_ = accumulatedCursorY;
    } else {
        cursorDeltaX_ = 0.0;
        cursorDeltaY_ = 0.0;
    }

    const double accumulatedScrollX = scrollDeltaX_ + state.scrollDeltaX;
    const double accumulatedScrollY = scrollDeltaY_ + state.scrollDeltaY;
    if (std::isfinite(accumulatedScrollX) && std::isfinite(accumulatedScrollY)) {
        scrollDeltaX_ = accumulatedScrollX;
        scrollDeltaY_ = accumulatedScrollY;
    } else {
        scrollDeltaX_ = 0.0;
        scrollDeltaY_ = 0.0;
    }
    cursorCaptured_ = state.cursorCaptured;
}

void InputTracker::beginCapture() noexcept {
    cursorCaptured_ = true;
    cursorDeltaX_ = 0.0;
    cursorDeltaY_ = 0.0;
    hasCursorPosition_ = false;
}

void InputTracker::endCapture() noexcept {
    cursorCaptured_ = false;
    cursorDeltaX_ = 0.0;
    cursorDeltaY_ = 0.0;
    hasCursorPosition_ = false;
}

void InputTracker::focusLost() noexcept {
    pressedKeys_ = 0U;
    releasedKeys_ |= heldKeys_;
    heldKeys_ = 0U;
    pressedMouseButtons_ = 0U;
    releasedMouseButtons_ |= heldMouseButtons_;
    heldMouseButtons_ = 0U;
    cursorDeltaX_ = 0.0;
    cursorDeltaY_ = 0.0;
    scrollDeltaX_ = 0.0;
    scrollDeltaY_ = 0.0;
    hasCursorPosition_ = false;
    cursorCaptured_ = false;
}

InputState InputTracker::consume() noexcept {
    InputState state;
    state.heldKeys_ = heldKeys_;
    state.pressedKeys_ = pressedKeys_;
    state.releasedKeys_ = releasedKeys_;
    state.heldMouseButtons_ = heldMouseButtons_;
    state.pressedMouseButtons_ = pressedMouseButtons_;
    state.releasedMouseButtons_ = releasedMouseButtons_;
    state.cursorX = cursorX_;
    state.cursorY = cursorY_;
    state.cursorDeltaX = cursorDeltaX_;
    state.cursorDeltaY = cursorDeltaY_;
    state.scrollDeltaX = scrollDeltaX_;
    state.scrollDeltaY = scrollDeltaY_;
    state.cursorCaptured = cursorCaptured_;
    state.gamepads_ = gamepads_;

    pressedKeys_ = 0U;
    releasedKeys_ = 0U;
    pressedMouseButtons_ = 0U;
    releasedMouseButtons_ = 0U;
    cursorDeltaX_ = 0.0;
    cursorDeltaY_ = 0.0;
    scrollDeltaX_ = 0.0;
    scrollDeltaY_ = 0.0;
    for (GamepadState &gamepad : gamepads_) {
        gamepad.pressedButtons_ = 0U;
        gamepad.releasedButtons_ = 0U;
    }
    return state;
}

CameraInput mapCameraInput(const bool forwardKey, const bool backKey, const bool rightKey, const bool leftKey,
                           const bool upKey, const bool downKey, const bool lookRight, const bool lookLeft,
                           const bool lookUp, const bool lookDown) noexcept {
    return CameraInput{
        static_cast<float>(forwardKey) - static_cast<float>(backKey),
        static_cast<float>(rightKey) - static_cast<float>(leftKey),
        static_cast<float>(upKey) - static_cast<float>(downKey),
        static_cast<float>(lookRight) - static_cast<float>(lookLeft),
        static_cast<float>(lookUp) - static_cast<float>(lookDown),
    };
}

CameraInput mapCameraInput(const InputState &state, const float mouseSensitivityDegrees) noexcept {
    CameraInput input = mapCameraInput(state.held(InputKey::W), state.held(InputKey::S), state.held(InputKey::D),
                                       state.held(InputKey::A), state.held(InputKey::E), state.held(InputKey::Q),
                                       state.held(InputKey::Right), state.held(InputKey::Left),
                                       state.held(InputKey::Up), state.held(InputKey::Down));
    const GamepadState &gamepad = state.gamepad(0U);
    input.forward = std::clamp(input.forward + gamepad.axis(GamepadAxis::LeftY), -1.0F, 1.0F);
    input.up = std::clamp(input.up + gamepad.axis(GamepadAxis::RightTrigger) - gamepad.axis(GamepadAxis::LeftTrigger),
                          -1.0F, 1.0F);
    input.right = std::clamp(input.right + gamepad.axis(GamepadAxis::LeftX), -1.0F, 1.0F);
    input.yaw = std::clamp(input.yaw + gamepad.axis(GamepadAxis::RightX), -1.0F, 1.0F);
    input.pitch = std::clamp(input.pitch + gamepad.axis(GamepadAxis::RightY), -1.0F, 1.0F);

    if (!state.cursorCaptured || !std::isfinite(mouseSensitivityDegrees)) {
        return input;
    }

    const double yawDegrees = state.cursorDeltaX * static_cast<double>(mouseSensitivityDegrees);
    const double pitchDegrees = -state.cursorDeltaY * static_cast<double>(mouseSensitivityDegrees);
    constexpr double kMaxFloat = static_cast<double>(std::numeric_limits<float>::max());
    if (std::isfinite(yawDegrees) && std::isfinite(pitchDegrees) && std::abs(yawDegrees) <= kMaxFloat &&
        std::abs(pitchDegrees) <= kMaxFloat) {
        input.mouseYawDegrees = static_cast<float>(yawDegrees);
        input.mousePitchDegrees = static_cast<float>(pitchDegrees);
    }
    return input;
}

void applyCameraInput(Camera &camera, const CameraInput &input, const float dt) {
    if (!std::isfinite(input.forward) || !std::isfinite(input.right) || !std::isfinite(input.up) ||
        !std::isfinite(input.yaw) || !std::isfinite(input.pitch) || !std::isfinite(input.mouseYawDegrees) ||
        !std::isfinite(input.mousePitchDegrees) || !std::isfinite(dt) || dt < 0.0f) {
        throw std::runtime_error("Camera input and delta time must be finite; "
                                 "delta time must be non-negative");
    }

    Camera nextCamera = camera;
    nextCamera.rotate(input.mouseYawDegrees, input.mousePitchDegrees);
    const float movementMagnitude = std::min(std::hypot(input.forward, input.right, input.up), 1.0f);
    nextCamera.update(input.forward, input.right, input.up, input.yaw, input.pitch, dt, movementMagnitude);
    camera = nextCamera;
}

} // namespace ve
