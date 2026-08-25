#pragma once

#include <chrono>

namespace motion::agent
{
    [[nodiscard]] constexpr bool screensaver_idle_is_inhibited(
        bool displayRequired, bool ownScreensaverWasActive) noexcept
    {
        return displayRequired && !ownScreensaverWasActive;
    }

    [[nodiscard]] constexpr bool automatic_lock_idle_is_inhibited(
        bool displayRequired, bool systemRequired, bool ownScreensaverActive) noexcept
    {
        (void)ownScreensaverActive;
        return displayRequired || systemRequired;
    }

    [[nodiscard]] constexpr bool display_off_after_lock_is_due(
        bool enabled,
        std::chrono::milliseconds lockedFor,
        int delaySeconds,
        bool alreadyTriggered,
        bool retryDue) noexcept
    {
        return enabled && !alreadyTriggered && retryDue &&
            lockedFor >= std::chrono::seconds(delaySeconds);
    }
}
