#include "platform/InputActions.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ve {
namespace {

[[nodiscard]] constexpr bool isValidCode(const std::uint8_t code, const std::uint8_t count) noexcept {
    return code < count;
}

void validateBinding(const InputBinding &binding) {
    if (!std::isfinite(binding.scale) || !std::isfinite(binding.deadzone) || binding.deadzone < 0.0F ||
        binding.deadzone >= 1.0F) {
        throw std::invalid_argument("Input binding scale or deadzone is invalid");
    }

    if (binding.source != InputBindingSource::GamepadAxis && binding.deadzone != 0.0F) {
        throw std::invalid_argument("Digital input bindings cannot have a deadzone");
    }
    switch (binding.source) {
    case InputBindingSource::Key:
        if (binding.slot != 0U || !isValidCode(binding.code, static_cast<std::uint8_t>(InputKey::Count))) {
            throw std::invalid_argument("Input key binding is invalid");
        }
        return;
    case InputBindingSource::MouseButton:
        if (binding.slot != 0U || !isValidCode(binding.code, static_cast<std::uint8_t>(InputMouseButton::Count))) {
            throw std::invalid_argument("Mouse button binding is invalid");
        }
        return;
    case InputBindingSource::GamepadButton:
        if (binding.slot >= GamepadSlotCount ||
            !isValidCode(binding.code, static_cast<std::uint8_t>(GamepadButton::Count))) {
            throw std::invalid_argument("Gamepad button binding is invalid");
        }
        return;
    case InputBindingSource::GamepadAxis:
        if (binding.slot >= GamepadSlotCount || !isValidCode(binding.code, static_cast<std::uint8_t>(GamepadAxis::Count))) {
            throw std::invalid_argument("Gamepad axis binding is invalid");
        }
        return;
    }

    throw std::invalid_argument("Input binding source is invalid");
}

[[nodiscard]] float applyDeadzone(const float value, const float deadzone) noexcept {
    const float magnitude = std::fabs(value);
    if (magnitude <= deadzone) {
        return 0.0F;
    }

    const float rescaled = (magnitude - deadzone) / (1.0F - deadzone);
    return std::copysign(rescaled, value);
}

} // namespace

float InputActionState::value(const InputActionId action) const noexcept {
    return action.valid() ? values_[action.index()] : 0.0F;
}

bool InputActionState::held(const InputActionId action) const noexcept {
    return (held_ & actionMask(action)) != 0U;
}

bool InputActionState::pressed(const InputActionId action) const noexcept {
    return (pressed_ & actionMask(action)) != 0U;
}

bool InputActionState::released(const InputActionId action) const noexcept {
    return (released_ & actionMask(action)) != 0U;
}

void InputActionMap::bind(const InputActionId action, const InputBinding &binding) {
    if (!action.valid()) {
        throw std::invalid_argument("Input action ID is invalid");
    }
    validateBinding(binding);

    ActionBindings &actionBindings = actions_[action.index()];
    if (actionBindings.count == MaxBindingsPerAction) {
        throw std::length_error("Input action binding capacity exceeded");
    }
    actionBindings.bindings[actionBindings.count] = binding;
    ++actionBindings.count;
}

void InputActionMap::clear(const InputActionId action) noexcept {
    if (action.valid()) {
        actions_[action.index()].count = 0U;
    }
}

InputActionState InputActionMap::evaluate(const InputState &input) const noexcept {
    InputActionState result;

    for (std::size_t actionIndex = 0U; actionIndex < MaxActions; ++actionIndex) {
        const ActionBindings &actionBindings = actions_[actionIndex];
        double value = 0.0;
        bool held = false;
        bool pressed = false;
        bool released = false;

        for (std::size_t bindingIndex = 0U; bindingIndex < actionBindings.count; ++bindingIndex) {
            const InputBinding &binding = actionBindings.bindings[bindingIndex];
            switch (binding.source) {
            case InputBindingSource::Key: {
                const auto key = static_cast<InputKey>(binding.code);
                const bool isHeld = input.held(key);
                held = held || isHeld;
                pressed = pressed || input.pressed(key);
                released = released || input.released(key);
                if (isHeld) {
                    value += static_cast<double>(binding.scale);
                }
                break;
            }
            case InputBindingSource::MouseButton: {
                const auto button = static_cast<InputMouseButton>(binding.code);
                const bool isHeld = input.held(button);
                held = held || isHeld;
                pressed = pressed || input.pressed(button);
                released = released || input.released(button);
                if (isHeld) {
                    value += static_cast<double>(binding.scale);
                }
                break;
            }
            case InputBindingSource::GamepadButton: {
                const GamepadState &gamepad = input.gamepad(binding.slot);
                const auto button = static_cast<GamepadButton>(binding.code);
                const bool isHeld = gamepad.held(button);
                held = held || isHeld;
                pressed = pressed || gamepad.pressed(button);
                released = released || gamepad.released(button);
                if (isHeld) {
                    value += static_cast<double>(binding.scale);
                }
                break;
            }
            case InputBindingSource::GamepadAxis: {
                const GamepadState &gamepad = input.gamepad(binding.slot);
                const double contribution =
                    static_cast<double>(applyDeadzone(
                        gamepad.axis(static_cast<GamepadAxis>(binding.code)), binding.deadzone)) *
                    static_cast<double>(binding.scale);
                value += contribution;
                held = held || contribution != 0.0F;
                break;
            }
            }
        }

        result.values_[actionIndex] =
            static_cast<float>(std::clamp(value, -1.0, 1.0));
        const std::uint64_t actionMask = std::uint64_t{1} << actionIndex;
        if (held) {
            result.held_ |= actionMask;
        }
        if (pressed) {
            result.pressed_ |= actionMask;
        }
        if (released) {
            result.released_ |= actionMask;
        }
    }

    return result;
}

} // namespace ve
