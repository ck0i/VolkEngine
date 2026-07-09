#pragma once

#include <memory>

#include <spdlog/logger.h>

namespace ve {

void initializeLogging();
std::shared_ptr<spdlog::logger>& logger();

} // namespace ve
