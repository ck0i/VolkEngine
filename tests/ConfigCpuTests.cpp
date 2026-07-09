#include "core/Config.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int gFailureCount = 0;

void expectTrue(const std::string_view context, const bool value) {
    if (!value) {
        std::cerr << "[FAILED] " << context << "\n";
        ++gFailureCount;
    }
}

void expectFalse(const std::string_view context, const bool value) {
    if (value) {
        std::cerr << "[FAILED] " << context << "\n";
        ++gFailureCount;
    }
}

} // namespace

int main() {
    expectTrue("default exposure value is valid", ve::isValidExposure(1.0f));
    expectTrue("positive finite exposure is valid", ve::isValidExposure(0.125f));
    expectFalse("zero exposure is invalid", ve::isValidExposure(0.0f));
    expectFalse("negative exposure is invalid", ve::isValidExposure(-1.0f));
    expectFalse("infinite exposure is invalid", ve::isValidExposure(std::numeric_limits<float>::infinity()));
    expectFalse("NaN exposure is invalid", ve::isValidExposure(std::nanf("")));

    if (gFailureCount == 0) {
        return 0;
    }
    std::cerr << "Config CPU tests failed: " << gFailureCount << " failure(s)\n";
    return 1;
}
