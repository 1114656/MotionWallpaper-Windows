#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace motion
{
    struct DisplayTarget
    {
        std::string id;
        std::wstring deviceName;
        std::wstring friendlyName;
        RECT bounds{};
        uint32_t refreshRateHz{ 60 };
        bool primary{};
    };

    [[nodiscard]] std::vector<DisplayTarget> enumerate_displays();
    [[nodiscard]] std::optional<RECT> find_display_bounds(std::wstring const& deviceName) noexcept;
    [[nodiscard]] RECT primary_display_bounds() noexcept;
}
