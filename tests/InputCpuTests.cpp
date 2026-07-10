#include "platform/Input.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

int gFailureCount = 0;

void expectNear(const std::string_view context, const float actual, const float expected, const float epsilon = 0.0001f) {
    if (std::fabs(actual - expected) > epsilon) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << '\n';
        ++gFailureCount;
    }
}

} // namespace

int main() {
    {
        const ve::CameraInput input = ve::mapCameraInput(true, true, true, false, false, true, true, false, false, true);
        expectNear("opposite movement keys cancel", input.forward, 0.0f);
        expectNear("right axis maps positively", input.right, 1.0f);
        expectNear("down axis maps negatively", input.up, -1.0f);
        expectNear("right look maps positively", input.yaw, 1.0f);
        expectNear("down look maps negatively", input.pitch, -1.0f);
    }

    {
        ve::Camera camera;
        const ve::Vec3 before = camera.position();
        ve::CameraInput input{};
        ve::applyCameraInput(camera, input, 1.0f);
        const ve::Vec3 after = camera.position();
        expectNear("zero input does not move x", after.x, before.x);
        expectNear("zero input does not move y", after.y, before.y);
        expectNear("zero input does not move z", after.z, before.z);
    }

    {
        ve::Camera camera;
        ve::CameraInput input{};
        input.forward = 1.0f;
        input.right = -1.0f;
        ve::applyCameraInput(camera, input, 1.0f);
        const ve::Vec3 displacement = camera.position() - ve::Vec3{0.0f, 1.6f, 5.0f};
        expectNear("diagonal movement stays normalized", length(displacement), 4.5f);
    }

    {
        ve::Camera camera;
        const ve::Vec3 before = camera.forward();
        ve::CameraInput input{};
        input.mouseYawDegrees = 12.0f;
        input.mousePitchDegrees = 8.0f;
        ve::applyCameraInput(camera, input, 0.0f);
        const ve::Vec3 after = camera.forward();
        expectTrue("mouse input changes camera orientation", std::fabs(before.x - after.x) > 0.0001f ||
                                                           std::fabs(before.y - after.y) > 0.0001f ||
                                                           std::fabs(before.z - after.z) > 0.0001f);
        expectTrue("positive mouse pitch points upward", after.y > before.y);
    }

    {
        ve::Camera camera;
        ve::CameraInput input{};
        input.mousePitchDegrees = 1000.0f;
        ve::applyCameraInput(camera, input, 0.0f);
        expectTrue("camera pitch remains clamped", camera.forward().y < 1.0f && camera.forward().y > 0.99f);
    }

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Input CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
