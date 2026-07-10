#pragma once

#include "core/Camera.hpp"

namespace ve {

struct CameraInput {
    float forward = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float mouseYawDegrees = 0.0f;
    float mousePitchDegrees = 0.0f;
};

[[nodiscard]] CameraInput mapCameraInput(bool forwardKey,
                                          bool backKey,
                                          bool rightKey,
                                          bool leftKey,
                                          bool upKey,
                                          bool downKey,
                                          bool lookRight,
                                          bool lookLeft,
                                          bool lookUp,
                                          bool lookDown) noexcept;

void applyCameraInput(Camera& camera, const CameraInput& input, float dt);

} // namespace ve
