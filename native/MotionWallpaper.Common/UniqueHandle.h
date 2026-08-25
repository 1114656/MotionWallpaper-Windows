#pragma once

#include <windows.h>

#include <utility>

namespace motion
{
    class unique_handle
    {
    public:
        unique_handle() = default;
        explicit unique_handle(HANDLE value) noexcept : value_(value) {}
        ~unique_handle() { reset(); }
        unique_handle(unique_handle const&) = delete;
        unique_handle& operator=(unique_handle const&) = delete;
        unique_handle(unique_handle&& other) noexcept : value_(other.release()) {}
        unique_handle& operator=(unique_handle&& other) noexcept
        {
            if (this != &other) reset(other.release());
            return *this;
        }
        [[nodiscard]] HANDLE get() const noexcept { return value_; }
        explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
        [[nodiscard]] HANDLE release() noexcept { return std::exchange(value_, nullptr); }
        void reset(HANDLE value = nullptr) noexcept
        {
            if (*this) CloseHandle(value_);
            value_ = value;
        }
    private:
        HANDLE value_{};
    };
}
