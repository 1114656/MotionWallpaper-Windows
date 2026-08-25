#pragma once

namespace motion::renderer
{
    [[nodiscard]] constexpr bool usable_desktop_host_bounds(
        bool visible,
        long left,
        long top,
        long right,
        long bottom,
        long virtualLeft,
        long virtualTop,
        long virtualRight,
        long virtualBottom) noexcept
    {
        return visible && left <= virtualLeft && top <= virtualTop &&
            right >= virtualRight && bottom >= virtualBottom;
    }
}
