#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ve {

struct ContentHash {
    std::array<std::byte, 32> bytes{};

    friend bool operator==(const ContentHash&, const ContentHash&) = default;
    friend auto operator<=>(const ContentHash&, const ContentHash&) = default;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string hex() const;
    [[nodiscard]] static ContentHash fromHex(std::string_view value);
};

[[nodiscard]] ContentHash hashBytes(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] ContentHash hashString(std::string_view value) noexcept;

} // namespace ve
