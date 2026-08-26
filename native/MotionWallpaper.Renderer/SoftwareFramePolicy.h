#pragma once

#include <algorithm>
#include <cstdint>

namespace motion::renderer
{
    [[nodiscard]] constexpr uint32_t default_software_frame_rate(uint32_t logicalProcessors) noexcept
    {
        return logicalProcessors >= 6 ? 60u : 30u;
    }

    [[nodiscard]] constexpr uint32_t software_probe_interval_ms(uint32_t frameRate) noexcept
    {
        if (!frameRate) return 0;
        // This is a hard presentation-rate ceiling, not an early decode probe.
        // Rounding to the nearest millisecond gives 17 ms for 60 FPS and
        // 33 ms for 30 FPS without a high-frequency busy retry.
        return (std::clamp)((1000u + frameRate / 2u) / frameRate, 1u, 1000u);
    }

    [[nodiscard]] constexpr uint32_t software_no_frame_retry_ms(uint32_t frameRate) noexcept
    {
        return (std::max)(2u, software_probe_interval_ms(frameRate) / 4u);
    }

    class SoftwareFrameGovernor
    {
    public:
        void Configure(uint32_t frameRate) noexcept
        {
            configuredFrameRate_ = frameRate;
            activeFrameRate_ = frameRate;
            slowFrames_ = 0;
            stableFrames_ = 0;
        }

        [[nodiscard]] uint32_t ActiveFrameRate() const noexcept { return activeFrameRate_; }

        // Presentation is the expensive WARP step. Repeated misses reduce only
        // the presentation rate; Media Engine remains clocked normally and
        // naturally returns the newest frame, so there is no bursty catch-up.
        [[nodiscard]] bool Observe(uint64_t processingMicroseconds) noexcept
        {
            if (!activeFrameRate_) return false;
            auto budget = 1'000'000ULL / activeFrameRate_;
            if (processingMicroseconds * 5 >= budget * 4) {
                stableFrames_ = 0;
                if (++slowFrames_ < 8) return false;
                slowFrames_ = 0;
                auto lower = activeFrameRate_ > 30 ? 30u : activeFrameRate_ > 24 ? 24u : activeFrameRate_;
                if (lower == activeFrameRate_) return false;
                activeFrameRate_ = lower;
                return true;
            }

            slowFrames_ = 0;
            if (activeFrameRate_ >= configuredFrameRate_ || processingMicroseconds * 2 > budget) {
                stableFrames_ = 0;
                return false;
            }
            if (++stableFrames_ < static_cast<uint64_t>(activeFrameRate_) * 10) return false;
            stableFrames_ = 0;
            activeFrameRate_ = configuredFrameRate_ <= 30 ? configuredFrameRate_ :
                activeFrameRate_ < 30 ? 30u : configuredFrameRate_;
            return true;
        }

    private:
        uint32_t configuredFrameRate_{};
        uint32_t activeFrameRate_{};
        uint32_t slowFrames_{};
        uint64_t stableFrames_{};
    };
}
