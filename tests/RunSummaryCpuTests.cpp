#include "core/FileSystem.hpp"
#include "core/RunSummary.hpp"

#include <cassert>
#include <filesystem>
#include <string>

int main() {
    ve::RunSummary summary{};
    summary.config.applicationName = "quoted \"name\"";
    summary.config.validation = true;
    summary.config.requireValidation = true;
    summary.config.initialWidth = 1920;
    summary.config.initialHeight = 1080;
    summary.config.depthPrepassMode = ve::DepthPrepassMode::ForceOn;
    summary.config.indirectSceneDraws = false;
    summary.options.resizeSmoke = true;
    summary.device.adapterName = "Test GPU";
    summary.options.scenarioName = "submission-pressure-v1";
    summary.options.warmupFrames = 20;
    summary.device.validationEnabled = true;
    summary.device.apiVersionMajor = 1;
    summary.device.apiVersionMinor = 3;
    summary.device.driverVersion = 42;
    summary.device.shaderDemoteToHelperInvocation = true;
    summary.stats.cpuFrameMs = 2.5;
    summary.stats.gpuTimestampsValid = false;
    summary.stats.drawCalls = 7;
    summary.stats.triangleCount = 1234;
    summary.stats.cpuGraphCompileMs = 0.75;
    summary.stats.graphPassCount = 4;
    summary.stats.graphResourceCount = 4;
    summary.stats.graphBarrierCount = 8;
    summary.stats.graphPhysicalAllocationCount = 2;
    summary.stats.graphTransientRequestedBytes = 1024;
    summary.stats.graphTransientAllocatedBytes = 1024;
    summary.stats.graphRecompileCount = 2;
    summary.stats.graphLastCompileWasResize = true;
    summary.stats.assetCookMs = 1.25;
    summary.stats.assetRecordCount = 6;
    summary.stats.assetCacheHits = 6;
    summary.stats.sceneClusterCount = 87;
    summary.stats.visibleCullingUnitCount = 1'540;
    summary.stats.testedCullingUnitCount = 2'048;
    summary.stats.occludedCullingUnitCount = 508;
    summary.stats.cullingUnitsAreClusters = true;
    summary.stats.materialDescriptorCount = 3;
    summary.stats.materialDescriptorCapacity = 4'096;
    summary.stats.gpuDrivenVisibility = true;
    summary.stats.localLightCount = 37;
    summary.stats.lightListOverflowCount = 5;
    summary.stats.shadowViewCount = 11;
  summary.stats.shadowAtlasCapacity = 16;
  summary.stats.shadowAtlasOverflowCount = 2;
  summary.stats.reflectionProbeCount = 3;
  summary.stats.materialClassCounts = {8U, 1U, 4U, 3U, 2U, 2U, 5U, 6U};
  summary.stats.shadowsEnabled = true;
  summary.stats.environmentMapEnabled = true;
  summary.stats.effectiveExposure = 1.25;
  summary.jobs.workerCount = 4U;
  summary.jobs.submitted = 12U;
  summary.jobs.succeeded = 11U;
  summary.jobs.failed = 1U;
  summary.jobs.queueHighWatermark = 5U;
  summary.jobs.executedNanoseconds = 2'500'000U;
  summary.jobs.steals = 3U;
  summary.jobs.categorySubmitted = {2U, 4U, 3U, 3U};
  ve::BoundedMetricSamples samples;
  for (std::uint32_t value = 1; value <= 100U; ++value) {
    samples.add(static_cast<double>(value));
    }
    summary.distributions.cpuFrame = samples.distribution();
    assert(summary.distributions.cpuFrame.sampleCount == 100U);
    assert(summary.distributions.cpuFrame.median == 50.5);
    assert(summary.distributions.cpuFrame.p95 > 95.0);
  assert(summary.distributions.cpuFrame.p99 > 99.0);
  summary.frameCount = 120;
  const std::string serialized = ve::serializeRunSummary(summary);
  assert(serialized.find("\"schema\":\"volkengine.run-summary\"") !=
         std::string::npos);
  assert(serialized.find("\"schema_version\":5") != std::string::npos);
  assert(serialized.find("\"scenario\":\"submission-pressure-v1\"") !=
         std::string::npos);
  assert(serialized.find("\"warmup_frames\":20") != std::string::npos);
  assert(serialized.find("\"hiz_occlusion\":true") != std::string::npos);
  assert(serialized.find("\"cluster_indirect_commands\":false") !=
         std::string::npos);
  assert(serialized.find("\"shadows\":true") != std::string::npos);
  assert(serialized.find("\"shader_demote_to_helper_invocation\":true") !=
         std::string::npos);
  assert(serialized.find("\"enabled\":true") != std::string::npos);
  assert(serialized.find("\"synchronization_validation\":true") !=
         std::string::npos);
  assert(serialized.find("\"frame_count\":120") != std::string::npos);
  assert(serialized.find("\"job_system\":{\"workers\":4,\"submitted\":12,"
                         "\"succeeded\":11,\"failed\":1") != std::string::npos);
  assert(
      serialized.find("\"categories\":{\"general\":2,\"simulation\":4,\"io\":3,"
                      "\"asset\":3}") != std::string::npos);
  assert(serialized.find("\"gpu_frame\":{\"available\":false,\"reason\":\"GPU "
                         "timestamp result unavailable\"") !=
         std::string::npos);
  assert(serialized.find("\"host_device_memory\":{\"available\":false") !=
         std::string::npos);
  assert(serialized.find("\"culling_unit\":\"cluster_instance\"") !=
         std::string::npos);
  assert(serialized.find("\"tested_units\":2048") != std::string::npos);
  assert(serialized.find("\"occluded_units\":508") != std::string::npos);
  assert(serialized.find("\"material_descriptors\":3") != std::string::npos);
  assert(serialized.find("\"material_descriptor_capacity\":4096") !=
         std::string::npos);
  assert(serialized.find("\"gpu_visibility_cull\":{\"available\":false") !=
         std::string::npos);
  assert(serialized.find("\"gpu_depth_pyramid\":{\"available\":false") !=
         std::string::npos);
  assert(
      serialized.find(
          "\"lighting\":{\"local_lights\":37,\"tile_overflow\":5,\"reflection_"
          "probes\":3,\"environment_map\":true,\"effective_exposure\":1.25") !=
      std::string::npos);
  assert(serialized.find("\"shadows\":{\"enabled\":true,\"views\":11,"
                         "\"capacity\":16,\"overflow\":2}") !=
         std::string::npos);
  assert(
      serialized.find(
          "\"material_classes\":{\"standard\":8,\"masked\":1,\"clear_coat\":4,"
          "\"foliage\":3,\"skin\":2,\"hair\":2,\"cloth\":5,\"emissive\":6}") !=
      std::string::npos);
  assert(serialized.find("\"gpu_light_assignment\":{\"available\":false") !=
         std::string::npos);
  assert(serialized.find("\"gpu_shadows\":{\"available\":false") !=
         std::string::npos);
  assert(serialized.find("\"frame_graph\":{\"passes\":4,\"logical_resources\":"
                         "4,\"barriers\":8") != std::string::npos);
  assert(serialized.find("\"last_recompile_reason\":\"resize\"") !=
         std::string::npos);
  assert(serialized.find("\"assets\":{\"records\":6,\"cache_hits\":6,\"cache_"
                         "misses\":0,\"rebuilt\":0}") != std::string::npos);
  assert(serialized.find("\"cpu_asset_cook\":{\"available\":true,\"value\":1."
                         "25,\"unit\":\"ms\"") != std::string::npos);
  assert(serialized.find("\"distributions\":{\"cpu_frame\":{\"unit\":\"ms\","
                         "\"sample_count\":100,\"available\":true") !=
         std::string::npos);
  ve::RunSummary disabledVisibility = summary;
  disabledVisibility.stats.gpuDrivenVisibility = false;
    disabledVisibility.stats.visibleCullingUnitCount = 0;
    disabledVisibility.stats.testedCullingUnitCount = 0;
    disabledVisibility.stats.occludedCullingUnitCount = 0;
  const std::string disabledSerialized =
      ve::serializeRunSummary(disabledVisibility);
  assert(disabledSerialized.find(
             "\"gpu_visibility\":{\"enabled\":false,\"validated\":false,"
             "\"culling_unit\":\"none\",\"visible_units\":0,\"tested_units\":0,"
             "\"occluded_units\":0") != std::string::npos);
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      (std::string{"volkengine-run-summary-test-"} +
         std::to_string(reinterpret_cast<std::uintptr_t>(&summary)));
    const std::filesystem::path output = directory / "nested" / "summary.json";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    ve::writeRunSummaryAtomic(output, summary);
    assert(ve::readTextFile(output) == serialized);
    std::filesystem::remove_all(directory, error);
    return 0;
}
