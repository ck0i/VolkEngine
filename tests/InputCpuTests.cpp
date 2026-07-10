#include "platform/Input.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

#include <limits>
#include <stdexcept>


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

template <typename F>
void expectThrowsRuntimeError(const std::string_view context, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error\n";
        ++gFailureCount;
    } catch (const std::runtime_error&) {
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": unexpected exception type\n";
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
        ve::InputState state;
        state.cursorCaptured = true;
        state.cursorDeltaX = 10.0;
        state.cursorDeltaY = -5.0;
        const ve::CameraInput mapped = ve::mapCameraInput(state);
        expectNear("snapshot cursor delta maps to camera yaw", mapped.mouseYawDegrees, 1.2f);
        expectNear("snapshot cursor delta maps to camera pitch", mapped.mousePitchDegrees, 0.6f);

        state.cursorDeltaX = std::numeric_limits<double>::max();
        const ve::CameraInput overflow = ve::mapCameraInput(state, std::numeric_limits<float>::max());
        expectNear("mouse conversion overflow drops yaw", overflow.mouseYawDegrees, 0.0f);
        expectNear("mouse conversion overflow drops pitch", overflow.mousePitchDegrees, 0.0f);
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

    {
        ve::Camera camera;
        const ve::Vec3 beforePosition = camera.position();
        const ve::Vec3 beforeForward = camera.forward();
        ve::CameraInput invalid{};
        invalid.mouseYawDegrees = std::numeric_limits<float>::quiet_NaN();
        expectThrowsRuntimeError("non-finite mouse input is rejected transactionally", [&] {
            ve::applyCameraInput(camera, invalid, 1.0f);
        });
        expectNear("rejected mouse input preserves position", camera.position().x, beforePosition.x);
        expectNear("rejected mouse input preserves orientation", camera.forward().x, beforeForward.x);

        invalid = {};
        invalid.mousePitchDegrees = 4.0f;
        expectThrowsRuntimeError("non-finite delta is rejected transactionally", [&] {
            ve::applyCameraInput(camera, invalid, std::numeric_limits<float>::quiet_NaN());
        });
        expectNear("rejected delta preserves position", camera.position().z, beforePosition.z);
        expectNear("rejected delta preserves orientation", camera.forward().y, beforeForward.y);

        invalid = {};
        invalid.forward = std::numeric_limits<float>::infinity();
        expectThrowsRuntimeError("non-finite movement input is rejected transactionally", [&] {
            ve::applyCameraInput(camera, invalid, 1.0f);
        });
        expectNear("rejected movement preserves position", camera.position().y, beforePosition.y);
        invalid = {};
        invalid.mousePitchDegrees = 4.0f;
        invalid.yaw = std::numeric_limits<float>::max();
        expectThrowsRuntimeError("camera-step overflow is rejected transactionally", [&] {
            ve::applyCameraInput(camera, invalid, 1.0f);
        });
        expectNear("rejected overflow preserves position", camera.position().x, beforePosition.x);
        expectNear("rejected overflow preserves orientation", camera.forward().z, beforeForward.z);

    }

    {
        ve::InputTracker tracker;
        tracker.keyEvent(ve::InputKey::W, true);
        tracker.keyEvent(ve::InputKey::W, true);
        tracker.mouseButtonEvent(ve::InputMouseButton::Left, true);
        tracker.beginCapture();
        tracker.cursorPosition(100.0, 200.0);
        tracker.cursorPosition(104.0, 197.0);

        const ve::InputState first = tracker.consume();
        expectTrue("held key appears in snapshot", first.held(ve::InputKey::W));
        expectTrue("initial key transition appears once", first.pressed(ve::InputKey::W));
        expectTrue("held mouse button appears in snapshot", first.held(ve::InputMouseButton::Left));
        expectTrue("initial mouse transition appears once", first.pressed(ve::InputMouseButton::Left));
        expectTrue("capture state appears in snapshot", first.cursorCaptured);
        expectTrue("cursor motion accumulates between consumes",
                   first.cursorDeltaX == 4.0 && first.cursorDeltaY == -3.0);

        const ve::InputState held = tracker.consume();
        expectTrue("held key persists across snapshots", held.held(ve::InputKey::W));
        expectTrue("key press edge is consumed once", !held.pressed(ve::InputKey::W));
        expectTrue("mouse press edge is consumed once", !held.pressed(ve::InputMouseButton::Left));
        expectTrue("cursor delta is consumed once", held.cursorDeltaX == 0.0 && held.cursorDeltaY == 0.0);

        tracker.keyEvent(ve::InputKey::W, false);
        tracker.mouseButtonEvent(ve::InputMouseButton::Left, false);
        const ve::InputState released = tracker.consume();
        expectTrue("released key is no longer held", !released.held(ve::InputKey::W));
        expectTrue("key release edge appears once", released.released(ve::InputKey::W));
        expectTrue("mouse release edge appears once", released.released(ve::InputMouseButton::Left));
    }

    {
        ve::InputTracker tracker;
        tracker.cursorPosition(12.0, 24.0);
        static_cast<void>(tracker.consume());
        tracker.cursorPosition(std::numeric_limits<double>::infinity(), 30.0);
        const ve::InputState ignored = tracker.consume();
        expectTrue("non-finite cursor samples preserve the last finite position",
                   ignored.cursorX == 12.0 && ignored.cursorY == 24.0);
        expectTrue("non-finite cursor samples do not emit motion",
                   ignored.cursorDeltaX == 0.0 && ignored.cursorDeltaY == 0.0);

        tracker.beginCapture();
        tracker.cursorPosition(-std::numeric_limits<double>::max(), 0.0);
        tracker.cursorPosition(std::numeric_limits<double>::max(), 0.0);
        const ve::InputState overflow = tracker.consume();
        expectTrue("cursor subtraction overflow preserves the latest position",
                   overflow.cursorX == std::numeric_limits<double>::max());
        expectTrue("cursor subtraction overflow resets accumulated motion",
                   overflow.cursorDeltaX == 0.0 && overflow.cursorDeltaY == 0.0);
    }

    {
        ve::InputTracker tracker;
        tracker.scrollEvent(1.0, 2.0);
        tracker.scrollEvent(-0.25, 3.0);
        const ve::InputState scrolled = tracker.consume();
        expectTrue("scroll offsets accumulate between snapshots",
                   scrolled.scrollDeltaX == 0.75 && scrolled.scrollDeltaY == 5.0);

        const ve::InputState consumed = tracker.consume();
        expectTrue("scroll offsets are consumed once",
                   consumed.scrollDeltaX == 0.0 && consumed.scrollDeltaY == 0.0);

        tracker.scrollEvent(std::numeric_limits<double>::infinity(), 1.0);
        const ve::InputState ignored = tracker.consume();
        expectTrue("non-finite scroll offsets are ignored",
                   ignored.scrollDeltaX == 0.0 && ignored.scrollDeltaY == 0.0);

        tracker.scrollEvent(std::numeric_limits<double>::max(), 0.0);
        tracker.scrollEvent(std::numeric_limits<double>::max(), 0.0);
        const ve::InputState overflow = tracker.consume();
        expectTrue("scroll accumulation overflow resets motion",
                   overflow.scrollDeltaX == 0.0 && overflow.scrollDeltaY == 0.0);
    }

    {
        ve::InputTracker tracker;
        tracker.keyEvent(ve::InputKey::Space, true);
        tracker.keyEvent(ve::InputKey::Space, false);
        const ve::InputState tapped = tracker.consume();
        expectTrue("between-frame tap preserves press edge", tapped.pressed(ve::InputKey::Space));
        expectTrue("between-frame tap preserves release edge", tapped.released(ve::InputKey::Space));
        expectTrue("between-frame tap reports final held state", !tapped.held(ve::InputKey::Space));
    }

    {
        ve::InputTracker frameInput;
        ve::InputTracker simulationInput;
        frameInput.keyEvent(ve::InputKey::Space, true);
        frameInput.beginCapture();
        frameInput.cursorPosition(10.0, 10.0);
        frameInput.cursorPosition(13.0, 8.0);
        frameInput.scrollEvent(0.0, 1.0);
        simulationInput.accumulate(frameInput.consume());
        simulationInput.accumulate(frameInput.consume());

        const ve::InputState firstStep = simulationInput.consume();
        expectTrue("pending input preserves key edge until a simulation step",
                   firstStep.pressed(ve::InputKey::Space));
        expectTrue("pending input preserves final held state",
                   firstStep.held(ve::InputKey::Space));
        expectTrue("pending input accumulates cursor motion across render frames",
                   firstStep.cursorDeltaX == 3.0 && firstStep.cursorDeltaY == -2.0);
        expectTrue("pending input accumulates scroll across render frames",
                   firstStep.scrollDeltaX == 0.0 && firstStep.scrollDeltaY == 1.0);

        const ve::InputState secondStep = simulationInput.consume();
        expectTrue("later fixed substep receives held input without repeated edge",
                   secondStep.held(ve::InputKey::Space) &&
                   !secondStep.pressed(ve::InputKey::Space));
        expectTrue("later fixed substep receives no repeated cursor motion",
                   secondStep.cursorDeltaX == 0.0 && secondStep.cursorDeltaY == 0.0);
        expectTrue("later fixed substep receives no repeated scroll motion",
                   secondStep.scrollDeltaX == 0.0 && secondStep.scrollDeltaY == 0.0);
    }

    {
        ve::InputTracker tracker;
        tracker.keyEvent(ve::InputKey::A, true);
        tracker.mouseButtonEvent(ve::InputMouseButton::Right, true);
        tracker.beginCapture();
        tracker.cursorPosition(4.0, 8.0);
        tracker.cursorPosition(9.0, 12.0);
        tracker.scrollEvent(1.0, -2.0);
        tracker.focusLost();

        const ve::InputState unfocused = tracker.consume();
        expectTrue("focus loss clears held keys", !unfocused.held(ve::InputKey::A));
        expectTrue("focus loss synthesizes key release", unfocused.released(ve::InputKey::A));
        expectTrue("focus loss clears held mouse buttons", !unfocused.held(ve::InputMouseButton::Right));
        expectTrue("focus loss synthesizes mouse release", unfocused.released(ve::InputMouseButton::Right));
        expectTrue("focus loss releases cursor capture", !unfocused.cursorCaptured);
        expectTrue("focus loss discards cursor motion",
                   unfocused.cursorDeltaX == 0.0 && unfocused.cursorDeltaY == 0.0);
        expectTrue("focus loss discards scroll motion",
                   unfocused.scrollDeltaX == 0.0 && unfocused.scrollDeltaY == 0.0);
    }

    {
        const ve::InputState empty;
        expectTrue("key sentinel query is safe", !empty.held(ve::InputKey::Count));
        expectTrue("out-of-range key query is safe", !empty.pressed(static_cast<ve::InputKey>(255U)));
        expectTrue("mouse sentinel query is safe", !empty.released(ve::InputMouseButton::Count));
        expectTrue("out-of-range mouse query is safe",
                   !empty.held(static_cast<ve::InputMouseButton>(255U)));
    }

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Input CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
