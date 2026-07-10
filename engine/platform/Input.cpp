#include "platform/Input.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>


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
        if (std::isfinite(deltaX) && std::isfinite(deltaY) &&
            std::isfinite(accumulatedX) && std::isfinite(accumulatedY)) {
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
    state.cursorCaptured = cursorCaptured_;

    pressedKeys_ = 0U;
    releasedKeys_ = 0U;
    pressedMouseButtons_ = 0U;
    releasedMouseButtons_ = 0U;
    cursorDeltaX_ = 0.0;
    cursorDeltaY_ = 0.0;
    return state;
}

CameraInput mapCameraInput(const bool forwardKey,
                           const bool backKey,
                           const bool rightKey,
                           const bool leftKey,
                           const bool upKey,
                           const bool downKey,
                           const bool lookRight,
                           const bool lookLeft,
                           const bool lookUp,
                           const bool lookDown) noexcept {
    return CameraInput{
        static_cast<float>(forwardKey) - static_cast<float>(backKey),
        static_cast<float>(rightKey) - static_cast<float>(leftKey),
        static_cast<float>(upKey) - static_cast<float>(downKey),
        static_cast<float>(lookRight) - static_cast<float>(lookLeft),
        static_cast<float>(lookUp) - static_cast<float>(lookDown),
    };
}

CameraInput mapCameraInput(const InputState& state, const float mouseSensitivityDegrees) noexcept {
    CameraInput input = mapCameraInput(
        state.held(InputKey::W), state.held(InputKey::S), state.held(InputKey::D), state.held(InputKey::A),
        state.held(InputKey::E), state.held(InputKey::Q), state.held(InputKey::Right), state.held(InputKey::Left),
        state.held(InputKey::Up), state.held(InputKey::Down));
    if (!state.cursorCaptured || !std::isfinite(mouseSensitivityDegrees)) {
        return input;
    }

    const double yawDegrees = state.cursorDeltaX * static_cast<double>(mouseSensitivityDegrees);
    const double pitchDegrees = -state.cursorDeltaY * static_cast<double>(mouseSensitivityDegrees);
    constexpr double kMaxFloat = static_cast<double>(std::numeric_limits<float>::max());
    if (std::isfinite(yawDegrees) && std::isfinite(pitchDegrees) &&
        std::abs(yawDegrees) <= kMaxFloat && std::abs(pitchDegrees) <= kMaxFloat) {
        input.mouseYawDegrees = static_cast<float>(yawDegrees);
        input.mousePitchDegrees = static_cast<float>(pitchDegrees);
    }
    return input;
}

void applyCameraInput(Camera& camera, const CameraInput& input, const float dt) {
    if (!std::isfinite(input.forward) || !std::isfinite(input.right) || !std::isfinite(input.up) ||
        !std::isfinite(input.yaw) || !std::isfinite(input.pitch) ||
        !std::isfinite(input.mouseYawDegrees) || !std::isfinite(input.mousePitchDegrees) ||
        !std::isfinite(dt) || dt < 0.0f) {
        throw std::runtime_error("Camera input and delta time must be finite; delta time must be non-negative");
    }

    Camera nextCamera = camera;
    nextCamera.rotate(input.mouseYawDegrees, input.mousePitchDegrees);
    nextCamera.update(input.forward, input.right, input.up, input.yaw, input.pitch, dt);
    camera = nextCamera;
}

} // namespace ve
