#include "platform/Input.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

#include <limits>
#include <stdexcept>

namespace {

int gFailureCount = 0;

void expectNear(const std::string_view context, const float actual, const float expected,
                const float epsilon = 0.0001f) {
    if (!std::isfinite(actual) || !std::isfinite(expected) || std::fabs(actual - expected) > epsilon) {
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

template <typename F> void expectThrowsRuntimeError(const std::string_view context, F &&callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error\n";
        ++gFailureCount;
    } catch (const std::runtime_error &) {
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": unexpected exception type\n";
        ++gFailureCount;
    }
}

constexpr std::uint16_t gamepadButtonMask(const ve::GamepadButton button) {
    return static_cast<std::uint16_t>(std::uint16_t{1} << static_cast<std::uint8_t>(button));
}

void setAxis(ve::GamepadSample &sample, const ve::GamepadAxis axis, const float value) {
    sample.axes[static_cast<std::size_t>(axis)] = value;
}

} // namespace

int main() {
    {
        const ve::CameraInput input =
            ve::mapCameraInput(true, true, true, false, false, true, true, false, false, true);
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
        expectThrowsRuntimeError("non-finite mouse input is rejected transactionally",
                                 [&] { ve::applyCameraInput(camera, invalid, 1.0f); });
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
        expectThrowsRuntimeError("non-finite movement input is rejected transactionally",
                                 [&] { ve::applyCameraInput(camera, invalid, 1.0f); });
        expectNear("rejected movement preserves position", camera.position().y, beforePosition.y);
        invalid = {};
        invalid.mousePitchDegrees = 4.0f;
        invalid.yaw = std::numeric_limits<float>::max();
        expectThrowsRuntimeError("camera-step overflow is rejected transactionally",
                                 [&] { ve::applyCameraInput(camera, invalid, 1.0f); });
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
        expectTrue("scroll offsets are consumed once", consumed.scrollDeltaX == 0.0 && consumed.scrollDeltaY == 0.0);

        tracker.scrollEvent(std::numeric_limits<double>::infinity(), 1.0);
        const ve::InputState ignored = tracker.consume();
        expectTrue("non-finite scroll offsets are ignored", ignored.scrollDeltaX == 0.0 && ignored.scrollDeltaY == 0.0);

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
        ve::GamepadSample firstRenderGamepad{};
        firstRenderGamepad.connected = true;
        firstRenderGamepad.buttons = gamepadButtonMask(ve::GamepadButton::A);
        setAxis(firstRenderGamepad, ve::GamepadAxis::LeftX, 0.25F);
        frameInput.gamepadSample(0U, firstRenderGamepad, {0.0F, 0.0F});
        simulationInput.accumulate(frameInput.consume());
        ve::GamepadSample secondRenderGamepad = firstRenderGamepad;
        setAxis(secondRenderGamepad, ve::GamepadAxis::LeftX, 0.75F);
        frameInput.gamepadSample(0U, secondRenderGamepad, {0.0F, 0.0F});
        simulationInput.accumulate(frameInput.consume());

        const ve::InputState firstStep = simulationInput.consume();
        expectTrue("pending input preserves key edge until a simulation step", firstStep.pressed(ve::InputKey::Space));
        expectTrue("pending input preserves final held state", firstStep.held(ve::InputKey::Space));
        expectTrue("pending input accumulates cursor motion across render frames",
                   firstStep.cursorDeltaX == 3.0 && firstStep.cursorDeltaY == -2.0);
        expectTrue("pending input accumulates scroll across render frames",
                   firstStep.scrollDeltaX == 0.0 && firstStep.scrollDeltaY == 1.0);
        expectTrue("pending gamepad edge survives two zero-step render samples",
                   firstStep.gamepad(0U).pressed(ve::GamepadButton::A) &&
                       firstStep.gamepad(0U).held(ve::GamepadButton::A));
        expectNear("pending gamepad level uses latest render sample",
                   firstStep.gamepad(0U).axis(ve::GamepadAxis::LeftX), 0.75F);

        const ve::InputState secondStep = simulationInput.consume();
        expectTrue("later fixed substep receives held input without repeated edge",
                   secondStep.held(ve::InputKey::Space) && !secondStep.pressed(ve::InputKey::Space));
        expectTrue("later fixed substep receives no repeated cursor motion",
                   secondStep.cursorDeltaX == 0.0 && secondStep.cursorDeltaY == 0.0);
        expectTrue("later fixed substep receives no repeated scroll motion",
                   secondStep.scrollDeltaX == 0.0 && secondStep.scrollDeltaY == 0.0);
        expectTrue("later fixed substep retains gamepad level without repeated edge",
                   secondStep.gamepad(0U).held(ve::GamepadButton::A) &&
                       !secondStep.gamepad(0U).pressed(ve::GamepadButton::A));
        expectNear("later fixed substep retains latest gamepad axis",
                   secondStep.gamepad(0U).axis(ve::GamepadAxis::LeftX), 0.75F);
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
        expectTrue("focus loss discards cursor motion", unfocused.cursorDeltaX == 0.0 && unfocused.cursorDeltaY == 0.0);
        expectTrue("focus loss discards scroll motion", unfocused.scrollDeltaX == 0.0 && unfocused.scrollDeltaY == 0.0);
    }

    {
        ve::InputTracker tracker;
        ve::GamepadSample sample{};
        sample.connected = true;
        tracker.gamepadSample(0U, sample);
        const ve::InputState centeredState = tracker.consume();
        const ve::GamepadState &centered = centeredState.gamepad(0U);
        expectNear("radial stick center is neutral", centered.axis(ve::GamepadAxis::LeftX), 0.0F);

        setAxis(sample, ve::GamepadAxis::LeftX, 0.15F);
        tracker.gamepadSample(0U, sample);
        const ve::InputState boundaryState = tracker.consume();
        const ve::GamepadState &boundary = boundaryState.gamepad(0U);
        expectNear("radial stick deadzone boundary is neutral", boundary.axis(ve::GamepadAxis::LeftX), 0.0F);

        setAxis(sample, ve::GamepadAxis::LeftX, 0.575F);
        tracker.gamepadSample(0U, sample);
        const ve::InputState rescaledState = tracker.consume();
        const ve::GamepadState &rescaled = rescaledState.gamepad(0U);
        expectNear("radial stick rescales outside deadzone", rescaled.axis(ve::GamepadAxis::LeftX), 0.5F);

        setAxis(sample, ve::GamepadAxis::LeftX, 0.5F);
        setAxis(sample, ve::GamepadAxis::LeftY, 0.5F);
        tracker.gamepadSample(0U, sample);
        const ve::InputState diagonalState = tracker.consume();
        const ve::GamepadState &diagonal = diagonalState.gamepad(0U);
        expectNear("radial diagonal preserves rescaled magnitude",
                   std::sqrt(diagonal.axis(ve::GamepadAxis::LeftX) * diagonal.axis(ve::GamepadAxis::LeftX) +
                             diagonal.axis(ve::GamepadAxis::LeftY) * diagonal.axis(ve::GamepadAxis::LeftY)),
                   (std::sqrt(0.5F) - 0.15F) / 0.85F);

        setAxis(sample, ve::GamepadAxis::LeftX, 2.0F);
        setAxis(sample, ve::GamepadAxis::LeftY, -2.0F);
        tracker.gamepadSample(0U, sample);
        const ve::InputState clampedState = tracker.consume();
        const ve::GamepadState &clamped = clampedState.gamepad(0U);
        expectNear("radial stick clamp bounds magnitude",
                   std::sqrt(clamped.axis(ve::GamepadAxis::LeftX) * clamped.axis(ve::GamepadAxis::LeftX) +
                             clamped.axis(ve::GamepadAxis::LeftY) * clamped.axis(ve::GamepadAxis::LeftY)),
                   1.0F);

        setAxis(sample, ve::GamepadAxis::LeftTrigger, 0.05F);
        setAxis(sample, ve::GamepadAxis::RightTrigger, 0.525F);
        tracker.gamepadSample(0U, sample);
        const ve::InputState triggersState = tracker.consume();
        const ve::GamepadState &triggers = triggersState.gamepad(0U);
        expectNear("trigger deadzone boundary is neutral", triggers.axis(ve::GamepadAxis::LeftTrigger), 0.0F);
        expectNear("trigger rescales outside deadzone", triggers.axis(ve::GamepadAxis::RightTrigger), 0.5F);
    }

    {
        ve::InputTracker tracker;
        ve::GamepadSample sample{};
        sample.connected = true;
        setAxis(sample, ve::GamepadAxis::LeftX, std::numeric_limits<float>::quiet_NaN());
        setAxis(sample, ve::GamepadAxis::LeftY, std::numeric_limits<float>::infinity());
        setAxis(sample, ve::GamepadAxis::RightX, -std::numeric_limits<float>::infinity());
        setAxis(sample, ve::GamepadAxis::RightY, std::numeric_limits<float>::quiet_NaN());
        setAxis(sample, ve::GamepadAxis::LeftTrigger, std::numeric_limits<float>::quiet_NaN());
        setAxis(sample, ve::GamepadAxis::RightTrigger, std::numeric_limits<float>::infinity());
        tracker.gamepadSample(0U, sample);
        const ve::InputState sanitizedState = tracker.consume();
        const ve::GamepadState &sanitized = sanitizedState.gamepad(0U);
        for (std::size_t axis = 0U; axis < static_cast<std::size_t>(ve::GamepadAxis::Count); ++axis) {
            expectNear("non-finite gamepad axes become neutral", sanitized.axis(static_cast<ve::GamepadAxis>(axis)),
                       0.0F);
        }

        setAxis(sample, ve::GamepadAxis::LeftX, 0.5F);
        setAxis(sample, ve::GamepadAxis::LeftY, 0.0F);
        setAxis(sample, ve::GamepadAxis::LeftTrigger, 0.5F);
        tracker.gamepadSample(0U, sample, {-1.0F, -std::numeric_limits<float>::infinity()});
        const ve::InputState invalidDeadzoneState = tracker.consume();
        const ve::GamepadState &invalidDeadzone = invalidDeadzoneState.gamepad(0U);
        expectNear("invalid stick deadzone clamps to zero", invalidDeadzone.axis(ve::GamepadAxis::LeftX), 0.5F);
        expectNear("invalid trigger deadzone clamps to zero", invalidDeadzone.axis(ve::GamepadAxis::LeftTrigger), 0.5F);

        tracker.gamepadSample(0U, sample, {1.0F, 1.0F});
        const ve::InputState fullDeadzoneState = tracker.consume();
        const ve::GamepadState &fullDeadzone = fullDeadzoneState.gamepad(0U);
        expectNear("full stick deadzone is neutral", fullDeadzone.axis(ve::GamepadAxis::LeftX), 0.0F);
        expectNear("full trigger deadzone is neutral", fullDeadzone.axis(ve::GamepadAxis::LeftTrigger), 0.0F);
    }

    {
        ve::InputTracker tracker;
        ve::GamepadSample sample{};
        sample.connected = true;
        sample.buttons = gamepadButtonMask(ve::GamepadButton::A);
        tracker.gamepadSample(0U, sample);
        const ve::InputState pressedState = tracker.consume();
        const ve::GamepadState &pressed = pressedState.gamepad(0U);
        expectTrue("gamepad press records held and press",
                   pressed.held(ve::GamepadButton::A) && pressed.pressed(ve::GamepadButton::A));

        tracker.focusLost();
        tracker.gamepadSample(0U, sample);
        const ve::InputState focusedState = tracker.consume();
        const ve::GamepadState &focusedGamepad = focusedState.gamepad(0U);
        expectTrue("focus loss preserves global gamepad lifetime",
                   focusedGamepad.connected() && focusedGamepad.held(ve::GamepadButton::A) &&
                       !focusedGamepad.pressed(ve::GamepadButton::A) && !focusedGamepad.released(ve::GamepadButton::A));

        tracker.gamepadSample(0U, sample);
        const ve::InputState stableState = tracker.consume();
        const ve::GamepadState &stable = stableState.gamepad(0U);
        expectTrue("same gamepad sample keeps held without duplicate press",
                   stable.held(ve::GamepadButton::A) && !stable.pressed(ve::GamepadButton::A));

        sample.buttons = 0U;
        tracker.gamepadSample(0U, sample);
        const ve::InputState releasedState = tracker.consume();
        const ve::GamepadState &released = releasedState.gamepad(0U);
        expectTrue("gamepad button release records once",
                   !released.held(ve::GamepadButton::A) && released.released(ve::GamepadButton::A));

        sample.buttons = gamepadButtonMask(ve::GamepadButton::A);
        setAxis(sample, ve::GamepadAxis::LeftX, 0.8F);
        tracker.gamepadSample(0U, sample);
        static_cast<void>(tracker.consume());
        tracker.gamepadSample(0U, {});
        const ve::InputState disconnectedState = tracker.consume();
        const ve::GamepadState &disconnected = disconnectedState.gamepad(0U);
        expectTrue("disconnect releases held button exactly once", !disconnected.connected() &&
                                                                       !disconnected.held(ve::GamepadButton::A) &&
                                                                       disconnected.released(ve::GamepadButton::A));
        expectNear("disconnect neutralizes axes", disconnected.axis(ve::GamepadAxis::LeftX), 0.0F);
        tracker.gamepadSample(0U, {});
        const ve::InputState repeatedlyDisconnectedState = tracker.consume();
        const ve::GamepadState &repeatedlyDisconnected = repeatedlyDisconnectedState.gamepad(0U);
        expectTrue("repeated disconnect has no duplicate release",
                   !repeatedlyDisconnected.released(ve::GamepadButton::A));

        tracker.gamepadSample(0U, sample);
        const ve::InputState reconnectedState = tracker.consume();
        const ve::GamepadState &reconnected = reconnectedState.gamepad(0U);
        expectTrue("reconnected held button becomes a press", reconnected.connected() &&
                                                                  reconnected.held(ve::GamepadButton::A) &&
                                                                  reconnected.pressed(ve::GamepadButton::A));
    }

    {
        ve::InputTracker tracker;
        ve::GamepadSample slotZero{};
        slotZero.connected = true;
        slotZero.buttons = gamepadButtonMask(ve::GamepadButton::A);
        setAxis(slotZero, ve::GamepadAxis::LeftX, -0.8F);
        ve::GamepadSample slotOne{};
        slotOne.connected = true;
        slotOne.buttons = gamepadButtonMask(ve::GamepadButton::B);
        setAxis(slotOne, ve::GamepadAxis::LeftX, -0.3F);
        ve::GamepadSample slotTwo{};
        slotTwo.connected = true;
        slotTwo.buttons = gamepadButtonMask(ve::GamepadButton::X);
        setAxis(slotTwo, ve::GamepadAxis::LeftX, 0.3F);
        ve::GamepadSample slotThree{};
        slotThree.connected = true;
        slotThree.buttons = gamepadButtonMask(ve::GamepadButton::Y);
        setAxis(slotThree, ve::GamepadAxis::LeftX, 0.8F);
        tracker.gamepadSample(0U, slotZero, {0.0F, 0.0F});
        tracker.gamepadSample(1U, slotOne, {0.0F, 0.0F});
        tracker.gamepadSample(2U, slotTwo, {0.0F, 0.0F});
        tracker.gamepadSample(3U, slotThree, {0.0F, 0.0F});
        const ve::InputState isolated = tracker.consume();
        expectTrue("slot zero retains its own button",
                   isolated.gamepad(0U).connected() && isolated.gamepad(0U).held(ve::GamepadButton::A));
        expectTrue("slot one retains its own button",
                   isolated.gamepad(1U).connected() && isolated.gamepad(1U).held(ve::GamepadButton::B));
        expectTrue("slot two retains its own button",
                   isolated.gamepad(2U).connected() && isolated.gamepad(2U).held(ve::GamepadButton::X));
        expectTrue("slot three retains its own button",
                   isolated.gamepad(3U).connected() && isolated.gamepad(3U).held(ve::GamepadButton::Y));
        expectNear("slot zero retains its own axis", isolated.gamepad(0U).axis(ve::GamepadAxis::LeftX), -0.8F);
        expectNear("slot one retains its own axis", isolated.gamepad(1U).axis(ve::GamepadAxis::LeftX), -0.3F);
        expectNear("slot two retains its own axis", isolated.gamepad(2U).axis(ve::GamepadAxis::LeftX), 0.3F);
        expectNear("slot three retains its own axis", isolated.gamepad(3U).axis(ve::GamepadAxis::LeftX), 0.8F);

        ve::GamepadSample invalidBit = slotZero;
        invalidBit.buttons = static_cast<std::uint16_t>(std::uint16_t{1} << 15U);
        tracker.gamepadSample(1U, invalidBit);
        tracker.gamepadSample(ve::GamepadSlotCount, slotThree);
        const ve::InputState safe = tracker.consume();
        expectTrue("invalid gamepad button bit is ignored",
                   !safe.gamepad(1U).held(ve::GamepadButton::A) && !safe.gamepad(1U).held(ve::GamepadButton::B));
        expectTrue("invalid gamepad slot leaves valid slots unchanged",
                   safe.gamepad(0U).held(ve::GamepadButton::A) && safe.gamepad(3U).held(ve::GamepadButton::Y));
        expectTrue("invalid gamepad button query is safe",
                   !safe.gamepad(0U).pressed(static_cast<ve::GamepadButton>(255U)));
        expectNear("invalid gamepad axis query is neutral", safe.gamepad(0U).axis(static_cast<ve::GamepadAxis>(255U)),
                   0.0F);
        expectTrue("invalid gamepad slot query is disconnected", !safe.gamepad(ve::GamepadSlotCount).connected());
    }

    {
        ve::InputTracker tracker;
        tracker.keyEvent(ve::InputKey::W, true);
        tracker.keyEvent(ve::InputKey::D, true);
        tracker.keyEvent(ve::InputKey::E, true);
        tracker.keyEvent(ve::InputKey::Right, true);
        tracker.keyEvent(ve::InputKey::Up, true);
        ve::GamepadSample sample{};
        sample.connected = true;
        setAxis(sample, ve::GamepadAxis::LeftX, -0.5F);
        setAxis(sample, ve::GamepadAxis::LeftY, 0.75F);
        setAxis(sample, ve::GamepadAxis::RightX, 0.5F);
        setAxis(sample, ve::GamepadAxis::RightY, -0.25F);
        setAxis(sample, ve::GamepadAxis::LeftTrigger, 0.3F);
        setAxis(sample, ve::GamepadAxis::RightTrigger, 0.8F);
        tracker.gamepadSample(0U, sample, {0.0F, 0.0F});
        const ve::CameraInput input = ve::mapCameraInput(tracker.consume());
        expectNear("slot zero left stick combines with clamped keyboard movement", input.forward, 1.0F);
        expectNear("slot zero left stick combines with keyboard strafe", input.right, 0.5F);
        expectNear("gamepad triggers combine with clamped keyboard vertical movement", input.up, 1.0F);
        expectNear("slot zero right stick combines with clamped keyboard yaw", input.yaw, 1.0F);
        expectNear("slot zero right stick combines with keyboard pitch", input.pitch, 0.75F);
    }

    {
        ve::Camera partialCamera;
        ve::Camera fullCamera;
        ve::Camera cappedDiagonalCamera;
        const ve::Vec3 initialPosition = partialCamera.position();

        ve::CameraInput partialInput{};
        partialInput.forward = 0.5F;
        ve::applyCameraInput(partialCamera, partialInput, 1.0F);

        ve::CameraInput fullInput{};
        fullInput.forward = 1.0F;
        ve::applyCameraInput(fullCamera, fullInput, 1.0F);

        ve::CameraInput diagonalInput{};
        diagonalInput.forward = 1.0F;
        diagonalInput.right = 1.0F;
        ve::applyCameraInput(cappedDiagonalCamera, diagonalInput, 1.0F);

        ve::Camera pitchedDigitalCamera;
        pitchedDigitalCamera.rotate(0.0F, 97.0F);
        const ve::Vec3 pitchedInitialPosition = pitchedDigitalCamera.position();
        ve::CameraInput pitchedDigitalInput{};
        pitchedDigitalInput.forward = 1.0F;
        pitchedDigitalInput.up = -1.0F;
        ve::applyCameraInput(pitchedDigitalCamera, pitchedDigitalInput, 1.0F);

        ve::Camera directCamera;
        const ve::Vec3 directInitialPosition = directCamera.position();
        directCamera.update(0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F);

        const float partialDistance = ve::length(partialCamera.position() - initialPosition);
        const float fullDistance = ve::length(fullCamera.position() - initialPosition);
        const float diagonalDistance = ve::length(cappedDiagonalCamera.position() - initialPosition);
        expectNear("partial analog movement preserves magnitude", partialDistance, fullDistance * 0.5F);
        expectNear("combined movement magnitude remains capped", diagonalDistance, fullDistance);
        expectNear("opposing pitched digital axes retain capped full movement",
                   ve::length(pitchedDigitalCamera.position() - pitchedInitialPosition), fullDistance);
        expectNear("direct camera update retains normalized movement contract",
                   ve::length(directCamera.position() - directInitialPosition), fullDistance);
    }

    {
        const ve::InputState empty;
        expectTrue("key sentinel query is safe", !empty.held(ve::InputKey::Count));
        expectTrue("out-of-range key query is safe", !empty.pressed(static_cast<ve::InputKey>(255U)));
        expectTrue("mouse sentinel query is safe", !empty.released(ve::InputMouseButton::Count));
        expectTrue("out-of-range mouse query is safe", !empty.held(static_cast<ve::InputMouseButton>(255U)));
    }

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Input CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
