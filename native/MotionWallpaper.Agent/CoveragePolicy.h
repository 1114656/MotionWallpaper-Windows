#pragma once

#include <cstdint>

namespace motion::agent
{
    struct WindowBounds
    {
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};
    };

    [[nodiscard]] constexpr bool covers_display(WindowBounds const& window, WindowBounds const& display) noexcept
    {
        constexpr int32_t tolerance = 2;
        return window.left <= display.left + tolerance && window.top <= display.top + tolerance &&
            window.right >= display.right - tolerance && window.bottom >= display.bottom - tolerance;
    }
}
