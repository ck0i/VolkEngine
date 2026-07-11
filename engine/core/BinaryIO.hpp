#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace ve {

template <typename T>
  requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
void appendLittleEndian(std::vector<std::byte> &output, const T value) {
  for (std::size_t index = 0U; index < sizeof(T); ++index) {
    output.push_back(static_cast<std::byte>(value >> (index * 8U)));
  }
}

template <typename T>
  requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
[[nodiscard]] T readLittleEndian(const std::span<const std::byte> bytes,
                                 const std::size_t offset) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::runtime_error("Little-endian value is truncated");
  }
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < sizeof(T); ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return static_cast<T>(value);
}

inline void appendLittleEndianFloat(std::vector<std::byte> &output,
                                    const float value) {
  static_assert(sizeof(float) == sizeof(std::uint32_t));
  static_assert(std::numeric_limits<float>::is_iec559);
  appendLittleEndian(output, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] inline float
readLittleEndianFloat(const std::span<const std::byte> bytes,
                      const std::size_t offset) {
  static_assert(sizeof(float) == sizeof(std::uint32_t));
  static_assert(std::numeric_limits<float>::is_iec559);
  return std::bit_cast<float>(readLittleEndian<std::uint32_t>(bytes, offset));
}

} // namespace ve
