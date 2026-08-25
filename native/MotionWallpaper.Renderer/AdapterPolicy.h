#pragma once

#include <cstdint>

namespace motion::renderer
{
    // Above roughly 4K60, the cost of decoding and copying a full-resolution
    // frame every refresh can saturate an integrated GPU. Preserve native
    // resolution and frame rate by preferring the high-performance adapter;
    // lighter media stays on the display adapter to minimize power use.
    [[nodiscard]] constexpr bool prefer_high_performance_adapter(
        uint32_t width,
        uint32_t height,
        uint32_t frameRateNumerator,
        uint32_t frameRateDenominator) noexcept
    {
        if (!width || !height || !frameRateNumerator || !frameRateDenominator) return false;
        constexpr uint64_t highWorkloadPixelsPerSecond =
            static_cast<uint64_t>(3840) * 2160 * 60;
        uint64_t pixelsPerSecond = static_cast<uint64_t>(width) * height * frameRateNumerator /
            frameRateDenominator;
        return pixelsPerSecond >= highWorkloadPixelsPerSecond;
    }
}
