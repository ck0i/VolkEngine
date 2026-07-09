#pragma once

#include <source_location>
#include <stdexcept>
#include <string>

namespace ve {

[[noreturn]] inline void failCheck(const char* expression, const std::source_location location = std::source_location::current()) {
    throw std::runtime_error(std::string("Check failed: ") + expression + " at " + location.file_name() + ":" + std::to_string(location.line()));
}

#define VE_CHECK(expr) do { if (!(expr)) { ::ve::failCheck(#expr); } } while (false)

} // namespace ve
