#pragma once

#include "../MotionWallpaper.Common/Common.h"

#include <cstdint>

namespace motion::agent
{
    inline constexpr uint32_t responsive_wait_ms = 50;
    inline constexpr uint32_t stable_wait_ms = 1000;

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

    // Renderer ACKs and short media-library transactions need a responsive
    // retry. Once the requested state is acknowledged, window, settings,
    // session and power events wake the Agent immediately; a one-second poll
    // is only a safety net and avoids a permanent 20 Hz policy loop.
    [[nodiscard]] constexpr uint32_t runtime_wait_interval_ms(
        bool targetReady, bool shortTransaction = false) noexcept
    {
        return targetReady && !shortTransaction ? stable_wait_ms : responsive_wait_ms;
    }

    // Transcoding is optional background work. Never start or continue it on
    // battery; the selected playback tier remains unchanged and any durable
    // request resumes when AC power returns.
    [[nodiscard]] constexpr bool variant_generation_allowed(
        bool onBattery, bool priorityRequest, bool playbackIdle) noexcept
    {
        return !onBattery && (priorityRequest || playbackIdle);
    }
}
