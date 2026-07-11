#include "core/RunSummary.hpp"

#include "core/FileSystem.hpp"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace ve {
namespace {

void appendEscaped(std::ostringstream& output, const std::string_view value) {
    output << '"';
    for (const char rawCharacter : value) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
}

const char* depthModeName(const DepthPrepassMode mode) noexcept {
    switch (mode) {
    case DepthPrepassMode::Auto: return "auto";
    case DepthPrepassMode::ForceOff: return "off";
    case DepthPrepassMode::ForceOn: return "on";
    }
    return "unavailable";
}

const char* buildType() noexcept {
#if VOLKENGINE_DEBUG_BUILD
    return "debug";
#else
    return "release";
#endif
}

const char* operatingSystem() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

const char* compilerName() noexcept {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
#define VE_STRINGIFY_IMPL(value) #value
#define VE_STRINGIFY(value) VE_STRINGIFY_IMPL(value)
    return "msvc " VE_STRINGIFY(_MSC_VER);
#else
    return "unknown";
#endif
}

std::string environmentOrUnavailable(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : "unavailable";
}

void appendMetric(std::ostringstream& output, const char* name, const double value,
                  const char* unit, const bool available, const char* reason) {
    appendEscaped(output, name);
    output << ":{";
    if (available && std::isfinite(value)) {
        output << "\"available\":true,\"value\":" << value;
    } else {
        output << "\"available\":false,\"reason\":";
        appendEscaped(output, reason);
    }
    output << ",\"unit\":";
    appendEscaped(output, unit);
    output << '}';
}

void appendDistribution(std::ostringstream& output, const char* name,
                        const MetricDistribution& distribution) {
    appendEscaped(output, name);
    output << ":{\"unit\":\"ms\",\"sample_count\":" << distribution.sampleCount;
    if (distribution.sampleCount == 0U) {
        output << ",\"available\":false,\"reason\":\"no post-warmup samples\"}";
        return;
    }
    output << ",\"available\":true,\"median\":" << distribution.median
           << ",\"p95\":" << distribution.p95
           << ",\"p99\":" << distribution.p99
           << ",\"max\":" << distribution.maximum
           << ",\"hitch_count\":" << distribution.hitchCount << '}';
}
} // namespace

std::string serializeRunSummary(const RunSummary& summary) {
    std::ostringstream output;
    output << std::setprecision(9);
    output << "{\n  \"schema\":\"volkengine.run-summary\",\n  \"schema_version\":"
           << RunSummary::kSchemaVersion << ",\n  \"revision\":";
    appendEscaped(output, environmentOrUnavailable("GITHUB_SHA"));
    output << ",\n  \"build\":{\"type\":";
    appendEscaped(output, buildType());
    output << ",\"os\":";
    appendEscaped(output, operatingSystem());
    output << ",\"compiler\":";
    appendEscaped(output, compilerName());
    output << "},\n  \"device\":{\"backend\":\"vulkan\",\"name\":";
    appendEscaped(output, summary.device.adapterName);
    output << ",\"vendor_id\":" << summary.device.vendorId
           << ",\"device_id\":" << summary.device.deviceId
           << ",\"driver_version\":" << summary.device.driverVersion
           << ",\"vulkan_api\":\"" << summary.device.apiVersionMajor << '.'
           << summary.device.apiVersionMinor << '.' << summary.device.apiVersionPatch << "\"},\n"
           << "  \"validation\":{\"requested\":" << std::boolalpha << summary.config.validation
           << ",\"required\":" << summary.config.requireValidation
           << ",\"enabled\":" << summary.device.validationEnabled
           << ",\"synchronization_validation\":"
           << (summary.config.requireValidation && summary.device.validationEnabled) << "},\n"
           << "  \"run\":{\"scenario\":";
    appendEscaped(output, summary.options.scenarioName);
    output << ",\"frame_count\":" << summary.frameCount
           << ",\"warmup_frames\":" << summary.options.warmupFrames
           << ",\"exit_status\":" << summary.exitStatus
           << ",\"resolution\":{\"width\":" << summary.config.initialWidth
           << ",\"height\":" << summary.config.initialHeight << "},\"variants\":{\"depth_prepass\":";
    appendEscaped(output, depthModeName(summary.config.depthPrepassMode));
    output << ",\"indirect_draws\":" << summary.config.indirectSceneDraws
           << ",\"gpu_visibility_validation\":"
           << summary.config.gpuVisibilityValidation
           << ",\"resize_smoke\":" << summary.options.resizeSmoke
           << ",\"acquire_recovery_smoke\":" << summary.options.acquireRecoverySmoke
           << ",\"screenshot\":" << !summary.options.screenshotPath.empty()
           << ",\"screenshot_frame\":" << summary.options.screenshotFrame << "}},\n"
           << "  \"counts\":{\"draws\":" << summary.stats.drawCalls
           << ",\"triangles\":" << summary.stats.triangleCount
           << ",\"scene_triangles\":" << summary.stats.sceneTriangleCount
           << ",\"instances\":" << summary.stats.sceneItemCount
           << ",\"visible_instances\":" << summary.stats.visibleItemCount
           << ",\"clusters\":" << summary.stats.sceneClusterCount
           << ",\"visible_cluster_instances\":"
           << summary.stats.visibleClusterInstanceCount
           << ",\"tested_cluster_instances\":"
           << summary.stats.testedClusterInstanceCount
           << ",\"occluded_cluster_instances\":"
           << summary.stats.occludedClusterInstanceCount
           << ",\"host_device_memory\":{\"available\":false,\"reason\":\"allocator budget is not exposed by the public renderer contract\"}},\n"
           << "  \"gpu_visibility\":{\"enabled\":"
           << summary.stats.gpuDrivenVisibility
           << ",\"validated\":" << summary.stats.gpuVisibilityValidated
           << ",\"material_descriptors\":"
           << summary.stats.materialDescriptorCount
           << ",\"material_descriptor_capacity\":"
           << summary.stats.materialDescriptorCapacity << "},\n"
           << "  \"frame_graph\":{\"passes\":" << summary.stats.graphPassCount
           << ",\"logical_resources\":" << summary.stats.graphResourceCount
           << ",\"barriers\":" << summary.stats.graphBarrierCount
           << ",\"physical_allocations\":" << summary.stats.graphPhysicalAllocationCount
           << ",\"transient_requested_bytes\":" << summary.stats.graphTransientRequestedBytes
           << ",\"transient_allocated_bytes\":" << summary.stats.graphTransientAllocatedBytes
           << ",\"recompile_count\":" << summary.stats.graphRecompileCount
           << ",\"last_recompile_reason\":\""
           << (summary.stats.graphLastCompileWasResize ? "resize" : "startup") << "\"},\n"
           << "  \"assets\":{\"records\":" << summary.stats.assetRecordCount
           << ",\"cache_hits\":" << summary.stats.assetCacheHits
           << ",\"cache_misses\":" << summary.stats.assetCacheMisses
           << ",\"rebuilt\":" << summary.stats.assetRebuiltCount << "},\n"
           << "  \"timings\":{";
    appendMetric(output, "cpu_frame", summary.stats.cpuFrameMs, "ms", true, "not recorded");
    output << ',';
    appendMetric(output, "cpu_scene_build", summary.stats.cpuSceneBuildMs, "ms", true, "not recorded");
    output << ',';
    appendMetric(output, "cpu_command_record", summary.stats.cpuCommandRecordMs, "ms", true, "not recorded");
    output << ',';
    appendMetric(output, "cpu_queue_submit", summary.stats.cpuQueueSubmitMs, "ms", true, "not recorded");
    output << ',';
    appendMetric(output, "cpu_graph_compile", summary.stats.cpuGraphCompileMs, "ms",
                 summary.stats.graphRecompileCount > 0U, "graph was not compiled");
    output << ',';
    appendMetric(output, "cpu_asset_cook", summary.stats.assetCookMs, "ms",
                 summary.stats.assetRecordCount > 0U, "asset pipeline was not executed");
    output << ',';
    appendMetric(output, "gpu_frame", summary.stats.gpuFrameMs, "ms", summary.stats.gpuTimestampsValid,
                 "GPU timestamp result unavailable");
    output << ',';
    appendMetric(output, "gpu_cluster_cull", summary.stats.gpuCullMs, "ms",
                 summary.stats.gpuTimestampsValid &&
                     summary.stats.gpuDrivenVisibility,
                 summary.stats.gpuDrivenVisibility
                     ? "GPU timestamp result unavailable"
                     : "pass disabled");
    output << ',';
    appendMetric(output, "gpu_depth_prepass", summary.stats.gpuDepthPrepassMs, "ms",
                 summary.stats.gpuTimestampsValid && summary.stats.depthPrepassEnabled,
                 summary.stats.depthPrepassEnabled ? "GPU timestamp result unavailable" : "pass disabled");
    output << ',';
    appendMetric(output, "gpu_hdr_scene", summary.stats.gpuHdrSceneMs, "ms", summary.stats.gpuTimestampsValid,
                 "GPU timestamp result unavailable");
    output << ',';
    appendMetric(output, "gpu_depth_pyramid",
                 summary.stats.gpuDepthPyramidMs, "ms",
                 summary.stats.gpuTimestampsValid &&
                     summary.stats.gpuDrivenVisibility,
                 summary.stats.gpuDrivenVisibility
                     ? "GPU timestamp result unavailable"
                     : "pass disabled");
    output << ',';
    appendMetric(output, "gpu_tone_map", summary.stats.gpuFinalPassMs, "ms", summary.stats.gpuTimestampsValid,
                 "GPU timestamp result unavailable");
    output << "},\n  \"distributions\":{";
    appendDistribution(output, "cpu_frame", summary.distributions.cpuFrame);
    output << ',';
    appendDistribution(output, "cpu_scene_build", summary.distributions.cpuSceneBuild);
    output << ',';
    appendDistribution(output, "cpu_command_record", summary.distributions.cpuCommandRecord);
    output << ',';
    appendDistribution(output, "cpu_queue_submit", summary.distributions.cpuQueueSubmit);
    output << ',';
    appendDistribution(output, "gpu_frame", summary.distributions.gpuFrame);
    output << "}\n}\n";
    return output.str();
}

void writeRunSummaryAtomic(const std::filesystem::path& path, const RunSummary& summary) {
    if (path.empty()) {
        throw std::invalid_argument("Run summary path must not be empty");
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::runtime_error(
                "Failed to create run summary directory " + parent.string() + ": " + error.message());
        }
    }
    const std::string serialized = serializeRunSummary(summary);
    const auto bytes = std::as_bytes(std::span{serialized.data(), serialized.size()});
    writeBinaryFileAtomic(path, bytes);
}

} // namespace ve
