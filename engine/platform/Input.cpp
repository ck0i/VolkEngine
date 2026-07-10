#include "platform/Input.hpp"

#include <cmath>
#include <stdexcept>


namespace ve {

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
