#pragma once

#include <string_view>

namespace motion::renderer
{
    enum class DecodePath { Hardware, Software, Unavailable };

    [[nodiscard]] constexpr DecodePath select_decode_path(
        std::wstring_view requestedMode, bool hardwareDecoderAvailable) noexcept
    {
        if (requestedMode == L"software") return DecodePath::Software;
        if (hardwareDecoderAvailable) return DecodePath::Hardware;
        return requestedMode == L"hardware" ? DecodePath::Unavailable : DecodePath::Software;
    }
}
