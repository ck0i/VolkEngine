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

    camera.setPosition({1.0f, 2.0f, 3.0f});
    expectTrue("finite camera position is accepted",
               camera.position().x == 1.0f && camera.position().y == 2.0f &&
                   camera.position().z == 3.0f);
    expectThrowsRuntimeError("non-finite camera position is rejected", [&] {
        camera.setPosition(
            {std::numeric_limits<float>::infinity(), 0.0f, 0.0f});
    });
    expectTrue("rejected camera position preserves state",
               camera.position().x == 1.0f && camera.position().y == 2.0f &&
                   camera.position().z == 3.0f);

    const ve::Mat4 baselineView = camera.viewMatrix();
    expectThrowsRuntimeError("NaN camera delta is rejected", [&] {
        camera.update(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN());
    });
    expectProjectionEqual("NaN camera delta preserves view", camera.viewMatrix(), baselineView);
    expectThrowsRuntimeError("negative camera delta is rejected", [&] {
        camera.update(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -0.01f);
    });
    expectProjectionEqual("negative camera delta preserves view", camera.viewMatrix(), baselineView);
    expectThrowsRuntimeError("infinite camera movement is rejected", [&] {
        camera.update(std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    });
    expectProjectionEqual("infinite camera movement preserves view", camera.viewMatrix(), baselineView);
    expectThrowsRuntimeError("NaN camera rotation is rejected", [&] {
        camera.rotate(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    });
    expectProjectionEqual("NaN camera rotation preserves view", camera.viewMatrix(), baselineView);
    expectThrowsRuntimeError("infinite camera rotation is rejected", [&] {
        camera.rotate(std::numeric_limits<float>::infinity(), 0.0f);
    });
    expectProjectionEqual("infinite camera rotation preserves view", camera.viewMatrix(), baselineView);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Camera CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
