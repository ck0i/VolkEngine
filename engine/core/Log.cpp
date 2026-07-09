#include "core/Log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace ve {
namespace {
std::shared_ptr<spdlog::logger> g_logger;
}

void initializeLogging() {
    if (g_logger) {
        return;
    }
    g_logger = spdlog::stdout_color_mt("VolkEngine");
    g_logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
#if VOLKENGINE_DEBUG_BUILD
    g_logger->set_level(spdlog::level::debug);
#else
    g_logger->set_level(spdlog::level::info);
#endif
    spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger>& logger() {
    initializeLogging();
    return g_logger;
}

} // namespace ve
