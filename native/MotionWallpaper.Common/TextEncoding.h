#pragma once

#include <windows.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace motion
{
    [[nodiscard]] inline std::string utf8_from_wide(std::wstring_view value)
    {
        if (value.empty()) return {};
        int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0) throw std::runtime_error("invalid UTF-16");
        std::string result(static_cast<size_t>(length), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr)) {
            throw std::runtime_error("UTF-16 conversion failed");
        }
        return result;
    }
}
