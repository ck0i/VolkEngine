#include "core/Time.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

int gFailureCount = 0;

void expectNear(const std::string_view context, const double actual, const double expected) {
    if (std::fabs(actual - expected) > 1.0e-12) {
        std::cerr << "[FAILED] " << context << ": expected " << expected << " but got " << actual << '\n';
        ++gFailureCount;
    }
}

} // namespace

int main() {
    expectNear("normal delta passes through", ve::clampDeltaSeconds(1.0 / 120.0, 0.05), 1.0 / 120.0);
    expectNear("hitch delta is capped", ve::clampDeltaSeconds(0.5, 0.05), 0.05);
    expectNear("negative delta is rejected", ve::clampDeltaSeconds(-0.25, 0.05), 0.0);
    expectNear("non-positive maximum disables simulation", ve::clampDeltaSeconds(0.05, 0.0), 0.0);

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Time CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
