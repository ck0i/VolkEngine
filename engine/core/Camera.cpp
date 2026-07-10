#include "core/Camera.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>


namespace ve {
namespace {

[[nodiscard]] bool finiteVec3(const Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finiteCameraInput(const float value) noexcept {
    return std::isfinite(value);
}

} // namespace

void Camera::setAspect(const float aspect) {
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        throw std::runtime_error("Camera aspect ratio must be finite and positive");
    }
    aspect_ = aspect;
}

void Camera::update(const float forwardInput, const float rightInput, const float upInput, const float yawDelta, const float pitchDelta, const float dt) {
    if (!finiteCameraInput(forwardInput) || !finiteCameraInput(rightInput) || !finiteCameraInput(upInput) ||
        !finiteCameraInput(yawDelta) || !finiteCameraInput(pitchDelta) || !finiteCameraInput(dt) || dt < 0.0f) {
        throw std::runtime_error("Camera inputs and delta time must be finite; delta time must be non-negative");
    }

    const float lookSensitivity = 90.0f;
    const float yawStep = yawDelta * lookSensitivity * dt;
    const float pitchStep = pitchDelta * lookSensitivity * dt;
    if (!std::isfinite(yawStep) || !std::isfinite(pitchStep)) {
        throw std::runtime_error("Camera rotation step is outside the finite range");
    }

    const float movementSpeed = 4.5f;
    const Vec3 movement = forward() * forwardInput + right() * rightInput + Vec3{0.0f, 1.0f, 0.0f} * upInput;
    if (!finiteVec3(movement)) {
        throw std::runtime_error("Camera movement input produced a non-finite vector");
    }
    const float movementLength = length(movement);
    if (!std::isfinite(movementLength)) {
        throw std::runtime_error("Camera movement length is outside the finite range");
    }

    Vec3 nextPosition = position_;
    if (movementLength > 0.0001f) {
        const Vec3 displacement = normalize(movement) * movementSpeed * dt;
        if (!finiteVec3(displacement)) {
            throw std::runtime_error("Camera movement step is outside the finite range");
        }
        nextPosition = position_ + displacement;
    }
    if (!finiteVec3(nextPosition)) {
        throw std::runtime_error("Camera position would become non-finite");
    }

    rotate(yawStep, pitchStep);
    position_ = nextPosition;
}

void Camera::rotate(const float yawDegrees, const float pitchDegrees) {
    if (!finiteCameraInput(yawDegrees) || !finiteCameraInput(pitchDegrees)) {
        throw std::runtime_error("Camera rotation must be finite");
    }
    const float nextYaw = yaw_ + yawDegrees;
    const float nextPitch = pitch_ + pitchDegrees;
    if (!std::isfinite(nextYaw) || !std::isfinite(nextPitch)) {
        throw std::runtime_error("Camera rotation would become non-finite");
    }
    yaw_ = nextYaw;
    pitch_ = std::clamp(nextPitch, -85.0f, 85.0f);
}

Vec3 Camera::forward() const {
    const float yawRad = radians(yaw_);
    const float pitchRad = radians(pitch_);
    return normalize({std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad), std::sin(yawRad) * std::cos(pitchRad)});
}

Vec3 Camera::right() const {
    return normalize(cross(forward(), {0.0f, 1.0f, 0.0f}));
}

Mat4 Camera::viewMatrix() const {
    return lookAt(position_, position_ + forward(), {0.0f, 1.0f, 0.0f});
}

Mat4 Camera::projectionMatrix() const {
    return perspective(verticalFov_, aspect_, nearPlane_, farPlane_);
}

} // namespace ve
