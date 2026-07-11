#pragma once

#include "platform/Input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ve {

class InputActionId {
public:
    static constexpr std::uint8_t Count = 64U;

    constexpr explicit InputActionId(const std::uint8_t index) noexcept : index_(index) {}

    [[nodiscard]] constexpr std::uint8_t index() const noexcept { return index_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return index_ < Count; }

private:
    std::uint8_t index_ = Count;
};

static_assert(sizeof(InputActionId) == sizeof(std::uint8_t));

enum class InputBindingSource : std::uint8_t {
    Key,
    MouseButton,
    GamepadButton,
    GamepadAxis,
};

struct InputBinding {
    InputBindingSource source = InputBindingSource::Key;
    std::uint8_t code = 0U;
    std::uint8_t slot = 0U;
    float scale = 1.0F;
    float deadzone = 0.0F;

    [[nodiscard]] static constexpr std::uint8_t encodedSlot(const std::size_t slot) noexcept {
        return slot <= std::numeric_limits<std::uint8_t>::max()
                   ? static_cast<std::uint8_t>(slot)
                   : std::numeric_limits<std::uint8_t>::max();
    }

    [[nodiscard]] static constexpr InputBinding key(const InputKey key, const float scale = 1.0F) noexcept {
        return {InputBindingSource::Key, static_cast<std::uint8_t>(key), 0U, scale, 0.0F};
    }

    [[nodiscard]] static constexpr InputBinding mouseButton(const InputMouseButton button,
                                                            const float scale = 1.0F) noexcept {
        return {InputBindingSource::MouseButton, static_cast<std::uint8_t>(button), 0U, scale, 0.0F};
    }

    [[nodiscard]] static constexpr InputBinding gamepadButton(const std::size_t slot,
                                                               const GamepadButton button,
                                                               const float scale = 1.0F) noexcept {
        return {InputBindingSource::GamepadButton, static_cast<std::uint8_t>(button), encodedSlot(slot), scale, 0.0F};
    }

    [[nodiscard]] static constexpr InputBinding gamepadAxis(const std::size_t slot, const GamepadAxis axis,
                                                             const float scale = 1.0F,
                                                             const float deadzone = 0.0F) noexcept {
        return {InputBindingSource::GamepadAxis, static_cast<std::uint8_t>(axis), encodedSlot(slot), scale, deadzone};
    }
};

static_assert(sizeof(InputBinding) <= 12U);

class InputActionState {
public:
    [[nodiscard]] float value(InputActionId action) const noexcept;
    [[nodiscard]] bool held(InputActionId action) const noexcept;
    [[nodiscard]] bool pressed(InputActionId action) const noexcept;
    [[nodiscard]] bool released(InputActionId action) const noexcept;

private:
    friend class InputActionMap;

    [[nodiscard]] static constexpr std::uint64_t actionMask(const InputActionId action) noexcept {
        return action.valid() ? (std::uint64_t{1} << action.index()) : 0U;
    }

    std::array<float, InputActionId::Count> values_{};
    std::uint64_t held_ = 0U;
    std::uint64_t pressed_ = 0U;
    std::uint64_t released_ = 0U;
};

class InputActionMap {
public:
    static constexpr std::size_t MaxActions = InputActionId::Count;
    static constexpr std::size_t MaxBindingsPerAction = 8U;

    void bind(InputActionId action, const InputBinding &binding);
    void clear(InputActionId action) noexcept;

    [[nodiscard]] InputActionState evaluate(const InputState &input) const noexcept;

private:
    struct ActionBindings {
        std::array<InputBinding, MaxBindingsPerAction> bindings{};
        std::uint8_t count = 0U;
    };

    static_assert(sizeof(ActionBindings) <= (sizeof(InputBinding) * MaxBindingsPerAction) + alignof(InputBinding));

    std::array<ActionBindings, MaxActions> actions_{};
};

} // namespace ve
