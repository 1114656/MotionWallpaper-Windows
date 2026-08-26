#pragma once

#include <string_view>

namespace motion::renderer
{
    enum class DecodePath { Automatic, Hardware, Software };

    [[nodiscard]] constexpr DecodePath select_decode_path(std::wstring_view requestedMode) noexcept
    {
        if (requestedMode == L"software") return DecodePath::Software;
        if (requestedMode == L"hardware") return DecodePath::Hardware;
        return DecodePath::Automatic;
    }

    [[nodiscard]] constexpr bool allows_software_device_fallback(DecodePath path) noexcept
    {
        return path == DecodePath::Automatic;
    }
}
