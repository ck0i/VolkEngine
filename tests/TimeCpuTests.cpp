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

template <typename F>
void expectThrowsInvalidArgument(const std::string_view context, F&& callable) {
    try {
        callable();
        std::cerr << "[FAILED] " << context << ": expected invalid_argument but no exception thrown\n";
        ++gFailureCount;
    } catch (const std::invalid_argument&) {
        // expected
    } catch (...) {
        std::cerr << "[FAILED] " << context << ": expected invalid_argument but threw a different exception\n";
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

    {
        ve::FixedStepClock fixedClock{0.01, 0.1, 3U};
        const ve::FixedStepBatch partial = fixedClock.advance(0.005);
        expectTrue("partial fixed frame emits no step", partial.stepCount == 0U);
        expectNear("partial fixed frame retains time", partial.retainedSeconds, 0.005);
        expectNear("partial fixed frame interpolation", partial.interpolationAlpha, 0.5);

        const ve::FixedStepBatch completed = fixedClock.advance(0.005);
        expectTrue("two partial frames emit one fixed step", completed.stepCount == 1U);
        expectNear("first fixed step reports fixed elapsed time", completed.firstStepElapsedSeconds, 0.01);
        expectNear("fixed step duration remains constant", completed.stepSeconds, 0.01);
        expectNear("completed fixed step consumes remainder", completed.retainedSeconds, 0.0);

        fixedClock.reset();
        const double belowStep = std::nextafter(0.01, 0.0);
        const ve::FixedStepBatch boundary = fixedClock.advance(belowStep);
        expectTrue("time immediately below one step emits no update", boundary.stepCount == 0U);
        expectNear("substep boundary does not create simulation time", boundary.retainedSeconds, belowStep);
    }

    {
        ve::FixedStepClock fixedClock{0.01, 0.1, 3U};
        const ve::FixedStepBatch hitch = fixedClock.advance(0.08);
        expectTrue("hitch obeys substep budget", hitch.stepCount == 3U);
        expectNear("hitch retains unconsumed simulation debt", hitch.retainedSeconds, 0.05);
        expectNear("hitch advances only executed steps", fixedClock.elapsedSeconds(), 0.03);
        expectNear("backlogged interpolation is bounded", hitch.interpolationAlpha, 1.0);

        const ve::FixedStepBatch catchUp = fixedClock.advance(0.0);
        expectTrue("retained debt catches up on later frame", catchUp.stepCount == 3U);
        expectNear("catch-up retains remaining debt", catchUp.retainedSeconds, 0.02);
        expectNear("catch-up elapsed remains monotonic", fixedClock.elapsedSeconds(), 0.06);

        const ve::FixedStepBatch drained = fixedClock.advance(0.0);
        expectTrue("remaining debt drains deterministically", drained.stepCount == 2U);
        expectNear("drained fixed clock has no remainder", drained.retainedSeconds, 0.0);
        expectNear("drained fixed clock recovers full accepted time", fixedClock.elapsedSeconds(), 0.08);
    }

    {
        ve::FixedStepClock fixedClock{0.01, 0.1, 3U};
        const ve::FixedStepBatch capped = fixedClock.advance(0.5);
        expectTrue("accumulator cap still obeys substep budget", capped.stepCount == 3U);
        expectNear("accumulator cap reports dropped wall time", capped.droppedSeconds, 0.4);
        expectNear("accumulator cap retains bounded debt", capped.retainedSeconds, 0.07);
        fixedClock.reset();
        expectNear("fixed clock reset clears elapsed time", fixedClock.elapsedSeconds(), 0.0);
        expectNear("fixed clock reset clears retained time", fixedClock.retainedSeconds(), 0.0);

        const ve::FixedStepBatch invalid = fixedClock.advance(std::numeric_limits<double>::quiet_NaN());
        expectTrue("non-finite frame delta emits no steps", invalid.stepCount == 0U);
        expectNear("non-finite frame delta retains no time", invalid.retainedSeconds, 0.0);
    }

    {
        const double maximum = std::numeric_limits<double>::max();
        ve::FixedStepClock fixedClock{maximum / 4.0, maximum / 2.0, 2U};
        static_cast<void>(fixedClock.advance(maximum / 2.0));
        const ve::FixedStepBatch saturated = fixedClock.advance(maximum / 2.0);
        expectTrue("extreme fixed-step batch emits both representable steps", saturated.stepCount == 2U);
        expectTrue("extreme first callback timestamp remains finite",
                   std::isfinite(saturated.elapsedSecondsForStep(0U)));
        expectTrue("extreme later callback timestamp saturates instead of overflowing",
                   std::isfinite(saturated.elapsedSecondsForStep(1U)) &&
                   saturated.elapsedSecondsForStep(1U) == maximum);
    }

    expectThrowsInvalidArgument("zero fixed step is rejected", [] {
        ve::FixedStepClock invalid{0.0, 0.1, 3U};
        (void)invalid;
    });
    expectThrowsInvalidArgument("short fixed accumulator is rejected", [] {
        ve::FixedStepClock invalid{0.02, 0.01, 3U};
        (void)invalid;
    });
    expectThrowsInvalidArgument("precision-stalled accumulator is rejected", [] {
        ve::FixedStepClock invalid{0.01, std::numeric_limits<double>::max(), 3U};
        (void)invalid;
    });
    expectThrowsInvalidArgument("zero fixed substep budget is rejected", [] {
        ve::FixedStepClock invalid{0.01, 0.1, 0U};
        (void)invalid;
    });

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Time CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
