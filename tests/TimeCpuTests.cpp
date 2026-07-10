#include "core/Time.hpp"

#include <cmath>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <limits>
namespace {

int gFailureCount = 0;

void expectNear(const std::string_view context, const double actual, const double expected) {
    if (std::fabs(actual - expected) > 1.0e-12) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << '\n';
        ++gFailureCount;
    }
}

template <typename F>
void expectThrowsRuntimeError(const std::string_view context, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected runtime_error but no exception thrown\n";
        ++gFailureCount;
    } catch (const std::runtime_error&) {
        // expected
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected runtime_error but threw a different exception\n";
        ++gFailureCount;
    }
}

} // namespace

int main() {
    expectNear("normal delta passes through", ve::clampDeltaSeconds(1.0 / 120.0, 0.05), 1.0 / 120.0);
    using Clock = std::chrono::steady_clock;
    const Clock::time_point anchor{};
    ve::Clock clock{anchor};
    const ve::FrameTiming first = clock.tickAt(anchor);
    expectNear("anchored first delta is zero", first.deltaSeconds, 0.0);
    expectNear("anchored first elapsed is zero", first.elapsedSeconds, 0.0);
    if (first.frameIndex != 0U) {
        std::cerr << "[FAILED] anchored first frame index expected 0 but got " << first.frameIndex << '\n';
        ++gFailureCount;
    }

    const ve::FrameTiming second = clock.tickAt(anchor + std::chrono::milliseconds(16));
    expectNear("anchored 16ms delta", second.deltaSeconds, 0.016);
    expectNear("anchored 16ms elapsed", second.elapsedSeconds, 0.016);
    if (second.frameIndex != 1U) {
        std::cerr << "[FAILED] anchored second frame index expected 1 but got " << second.frameIndex << '\n';
        ++gFailureCount;
    }

    const ve::FrameTiming third = clock.tickAt(anchor + std::chrono::milliseconds(40));
    expectNear("anchored accumulated delta", third.deltaSeconds, 0.024);
    expectNear("anchored accumulated elapsed", third.elapsedSeconds, 0.040);
    if (third.frameIndex != 2U) {
        std::cerr << "[FAILED] anchored third frame index expected 2 but got " << third.frameIndex << '\n';
        ++gFailureCount;
    }

    expectThrowsRuntimeError("backward clock sample is rejected", [&] {
        (void)clock.tickAt(anchor + std::chrono::milliseconds(24));
    });
    const ve::FrameTiming afterRejectedSample = clock.tickAt(anchor + std::chrono::milliseconds(56));
    expectNear("clock state survives rejected sample", afterRejectedSample.deltaSeconds, 0.016);
    expectNear("clock elapsed survives rejected sample", afterRejectedSample.elapsedSeconds, 0.056);
    if (afterRejectedSample.frameIndex != 3U) {
        std::cerr << "[FAILED] post-rejection frame index expected 3 but got " << afterRejectedSample.frameIndex << '\n';
        ++gFailureCount;
    }
    expectNear("hitch delta is capped", ve::clampDeltaSeconds(0.5, 0.05), 0.05);
    expectNear("negative delta is rejected", ve::clampDeltaSeconds(-0.25, 0.05), 0.0);
    expectNear("non-positive maximum disables simulation", ve::clampDeltaSeconds(0.05, 0.0), 0.0);
    expectNear("infinite delta is rejected",
               ve::clampDeltaSeconds(std::numeric_limits<double>::infinity(), 0.05),
               0.0);
    expectNear("infinite maximum disables simulation",
               ve::clampDeltaSeconds(0.05, std::numeric_limits<double>::infinity()),
               0.0);
    const double simulationAfterNormal = ve::advanceSimulationSeconds(0.0, 0.016, 0.05);
    expectNear("simulation time advances by normal delta", simulationAfterNormal, 0.016);
    expectNear("simulation hitch is capped", ve::advanceSimulationSeconds(simulationAfterNormal, 0.5, 0.05), 0.066);
    expectNear("simulation negative delta does not advance", ve::advanceSimulationSeconds(simulationAfterNormal, -0.25, 0.05), simulationAfterNormal);
    expectNear("simulation NaN delta does not advance",
               ve::advanceSimulationSeconds(simulationAfterNormal, std::numeric_limits<double>::quiet_NaN(), 0.05),
               simulationAfterNormal);
    const double saturatedSimulation = ve::advanceSimulationSeconds(
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max());
    expectTrue("finite simulation overflow saturates",
               std::isfinite(saturatedSimulation) &&
               saturatedSimulation == std::numeric_limits<double>::max());
    expectNear("infinite simulation step does not advance",
               ve::advanceSimulationSeconds(10.0,
                                            std::numeric_limits<double>::infinity(),
                                            std::numeric_limits<double>::infinity()),
               10.0);
    expectNear("non-finite simulation state resets safely",
               ve::advanceSimulationSeconds(std::numeric_limits<double>::quiet_NaN(), 0.016, 0.05),
               0.0);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Time CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
