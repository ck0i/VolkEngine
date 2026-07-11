#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ve {

struct MetricDistribution {
    std::uint32_t sampleCount = 0;
    double median = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double maximum = 0.0;
    std::uint32_t hitchCount = 0;
};

class BoundedMetricSamples {
public:
    static constexpr std::size_t kCapacity = 2048;

    void add(const double value) noexcept {
        if (!std::isfinite(value) || value < 0.0) return;
        samples_[cursor_] = value;
        cursor_ = (cursor_ + 1U) % samples_.size();
        count_ = std::min(count_ + 1U, samples_.size());
    }

    [[nodiscard]] MetricDistribution distribution() const {
        if (count_ == 0U) return {};
        std::vector<double> ordered;
        ordered.reserve(count_);
        if (count_ < samples_.size()) {
            ordered.insert(ordered.end(), samples_.begin(), samples_.begin() +
                           static_cast<std::ptrdiff_t>(count_));
        } else {
            ordered.insert(ordered.end(), samples_.begin() +
                           static_cast<std::ptrdiff_t>(cursor_), samples_.end());
            ordered.insert(ordered.end(), samples_.begin(), samples_.begin() +
                           static_cast<std::ptrdiff_t>(cursor_));
        }
        std::ranges::sort(ordered);
        const auto percentile = [&](const double quantile) {
            const double position = quantile * static_cast<double>(ordered.size() - 1U);
            const std::size_t lower = static_cast<std::size_t>(position);
            const std::size_t upper = std::min(lower + 1U, ordered.size() - 1U);
            const double fraction = position - static_cast<double>(lower);
            return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction;
        };
        MetricDistribution result;
        result.sampleCount = static_cast<std::uint32_t>(ordered.size());
        result.median = percentile(0.50);
        result.p95 = percentile(0.95);
        result.p99 = percentile(0.99);
        result.maximum = ordered.back();
        const double hitchThreshold = std::max(result.median * 2.0, 16.667);
        result.hitchCount = static_cast<std::uint32_t>(
            std::ranges::count_if(ordered, [&](const double sample) {
                return sample > hitchThreshold;
            }));
        return result;
    }

private:
    std::array<double, kCapacity> samples_{};
    std::size_t cursor_ = 0;
    std::size_t count_ = 0;
};

struct RunMetricDistributions {
    MetricDistribution cpuFrame;
    MetricDistribution cpuSceneBuild;
    MetricDistribution cpuCommandRecord;
    MetricDistribution cpuQueueSubmit;
    MetricDistribution gpuFrame;
};

} // namespace ve
