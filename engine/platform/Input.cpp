#include "platform/Input.hpp"

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
    camera.rotate(input.mouseYawDegrees, input.mousePitchDegrees);
    camera.update(input.forward, input.right, input.up, input.yaw, input.pitch, dt);
}

} // namespace ve
