#pragma once

#include "../MotionWallpaper.Common/Common.h"

#include <cstdint>

namespace motion::agent
{
    enum class RuntimeAction
    {
        DisplayOff,
        Locked,
        ScreensaverPlay,
        Stopped,
        DesktopPaused,
        DesktopFrozen,
        DesktopPlay
    };

    struct RuntimeSignals
    {
        bool displayOn{ true };
        bool sessionLocked{};
        bool covered{};
        bool hasMedia{};
        int64_t idleSeconds{};
    };

    // This is the only playback priority reducer used by the Agent. Keep the
    // order explicit: power/session > screensaver > disabled/missing media >
    // full-screen coverage > activity freeze > normal playback.
    [[nodiscard]] inline RuntimeAction reduce_runtime_action(
        motion::Settings const& settings, RuntimeSignals const& signals) noexcept
    {
        if (!signals.displayOn) return RuntimeAction::DisplayOff;
        if (signals.sessionLocked) return RuntimeAction::Locked;
        if (signals.hasMedia && settings.screensaverEnabled &&
            signals.idleSeconds >= settings.idleTimeoutSeconds) {
            return RuntimeAction::ScreensaverPlay;
        }
        if (!settings.desktopPlayback || !signals.hasMedia) return RuntimeAction::Stopped;
        if (signals.covered && !settings.continueWhenCovered) return RuntimeAction::DesktopPaused;
        if (!settings.activePlaybackEnabled) return RuntimeAction::DesktopFrozen;
        return RuntimeAction::DesktopPlay;
    }
}
