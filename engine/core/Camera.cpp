#include "core/Camera.hpp"

#include <algorithm>
#include <cmath>

namespace ve {

void Camera::update(const float forwardInput, const float rightInput, const float upInput, const float yawDelta, const float pitchDelta, const float dt) {
    const float lookSensitivity = 90.0f;
    rotate(yawDelta * lookSensitivity * dt, pitchDelta * lookSensitivity * dt);

    const float movementSpeed = 4.5f;
    const Vec3 movement = forward() * forwardInput + right() * rightInput + Vec3{0.0f, 1.0f, 0.0f} * upInput;
    if (length(movement) > 0.0001f) {
        position_ = position_ + normalize(movement) * movementSpeed * dt;
    }
}

void Camera::rotate(const float yawDegrees, const float pitchDegrees) {
    yaw_ += yawDegrees;
    pitch_ = std::clamp(pitch_ + pitchDegrees, -85.0f, 85.0f);
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
