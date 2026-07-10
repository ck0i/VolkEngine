#pragma once

#include "core/Config.hpp"

#include <cstddef>

namespace ve {

struct FrameGraphVariantPolicy {
    static constexpr std::size_t kVariantCount = 4;

    [[nodiscard]] static constexpr std::size_t index(const bool depthPrepass, const bool screenshot) noexcept {
        return (depthPrepass ? 1U : 0U) | (screenshot ? 2U : 0U);
    }

    [[nodiscard]] static constexpr bool depthVariantAvailable(const DepthPrepassMode mode, const bool depthPrepass) noexcept {
        return depthPrepass ? mode != DepthPrepassMode::ForceOff : mode != DepthPrepassMode::ForceOn;
    }
};

} // namespace ve
