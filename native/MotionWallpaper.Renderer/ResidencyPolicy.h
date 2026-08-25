#pragma once

#include <cstdint>

namespace motion::renderer
{
    inline constexpr uint32_t residency_memory_check_ms = 30'000;
    inline constexpr uint32_t residency_low_memory_delay_ms = 250;

    [[nodiscard]] constexpr uint32_t residency_timer_delay_ms(bool lowMemory) noexcept
    {
        return lowMemory ? residency_low_memory_delay_ms : residency_memory_check_ms;
    }

    [[nodiscard]] constexpr bool should_compact_idle(bool lowMemory) noexcept { return lowMemory; }
}
