#pragma once

#include "core/Math.hpp"

namespace ve {

class Camera {
public:
    void setAspect(float aspect);
    void update(float forward, float right, float up, float yawDelta, float pitchDelta, float dt,
                float movementMagnitude = 1.0f);
    void rotate(float yawDegrees, float pitchDegrees);

    [[nodiscard]] Mat4 viewMatrix() const;
    [[nodiscard]] Mat4 projectionMatrix() const;
    [[nodiscard]] Mat4 viewProjectionMatrix() const { return projectionMatrix() * viewMatrix(); }
    [[nodiscard]] Vec3 position() const { return position_; }
    [[nodiscard]] Vec3 forward() const;
    [[nodiscard]] Vec3 right() const;
    [[nodiscard]] float verticalFov() const noexcept { return verticalFov_; }
    [[nodiscard]] float aspect() const noexcept { return aspect_; }
    [[nodiscard]] float nearPlane() const noexcept { return nearPlane_; }
    [[nodiscard]] float farPlane() const noexcept { return farPlane_; }

private:
    Vec3 position_{0.0f, 1.6f, 5.0f};
    float yaw_ = -90.0f;
    float pitch_ = -12.0f;
    float aspect_ = 16.0f / 9.0f;
    float verticalFov_ = radians(65.0f);
    float nearPlane_ = 0.05f;
    float farPlane_ = 500.0f;
};

} // namespace ve
