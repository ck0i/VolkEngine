#pragma once

#include "core/Config.hpp"
#include "core/MetricDistribution.hpp"
#include "renderer/Renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace ve {

struct RunSummary {
    static constexpr std::uint32_t kSchemaVersion = 3;

    EngineConfig config;
    RunOptions options;
    RenderDeviceInfo device;
    RenderStats stats;
    RunMetricDistributions distributions;
    std::uint64_t frameCount = 0;
    int exitStatus = 0;
};

[[nodiscard]] std::string serializeRunSummary(const RunSummary& summary);
void writeRunSummaryAtomic(const std::filesystem::path& path, const RunSummary& summary);

} // namespace ve
