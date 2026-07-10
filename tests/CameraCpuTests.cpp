#include "core/Camera.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

int gFailureCount = 0;

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << "\n";
        ++gFailureCount;
    }
}

template <typename F>
void expectThrowsRuntimeError(const std::string_view context, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error but no exception thrown\n";
        ++gFailureCount;
    } catch (const std::runtime_error&) {
        // expected
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected runtime_error but threw a different exception\n";
        ++gFailureCount;
    }
}

void expectProjectionEqual(const std::string_view context, const ve::Mat4& actual, const ve::Mat4& expected) {
    for (std::size_t index = 0; index < actual.m.size(); ++index) {
        if (actual.m[index] != expected.m[index]) {
            std::cerr << "[FAILED] " << context << ": matrix element " << index << " changed\n";
            ++gFailureCount;
            return;
        }
    }
}

} // namespace

int main() {
    ve::Camera camera;
    camera.setAspect(2.0f);
    const ve::Mat4 baselineProjection = camera.projectionMatrix();
    for (const float value : baselineProjection.m) {
        expectTrue("valid aspect produces finite projection", std::isfinite(value));
    }

    expectThrowsRuntimeError("zero aspect is rejected", [&] { camera.setAspect(0.0f); });
    expectProjectionEqual("zero aspect preserves projection", camera.projectionMatrix(), baselineProjection);
    expectThrowsRuntimeError("negative aspect is rejected", [&] { camera.setAspect(-1.0f); });
    expectProjectionEqual("negative aspect preserves projection", camera.projectionMatrix(), baselineProjection);
    expectThrowsRuntimeError("NaN aspect is rejected", [&] { camera.setAspect(std::numeric_limits<float>::quiet_NaN()); });
    expectProjectionEqual("NaN aspect preserves projection", camera.projectionMatrix(), baselineProjection);
    expectThrowsRuntimeError("infinite aspect is rejected", [&] { camera.setAspect(std::numeric_limits<float>::infinity()); });
    expectProjectionEqual("infinite aspect preserves projection", camera.projectionMatrix(), baselineProjection);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Camera CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
