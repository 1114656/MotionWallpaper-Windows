#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Agent/RandomSelectionPolicy.h"
#include "../MotionWallpaper.Agent/RuntimePolicy.h"
#include "../MotionWallpaper.Agent/CoveragePolicy.h"
#include "../MotionWallpaper.Agent/IdlePolicy.h"
#include "../MotionWallpaper.Agent/PlaybackCapabilityPolicy.h"
#include "../MotionWallpaper.Agent/SharedRendererPolicy.h"
#include "../MotionWallpaper.Agent/VideoVariantPolicy.h"
#include "../MotionWallpaper.Agent/VideoTranscoder.h"
#include "../MotionWallpaper.Renderer/ResidencyPolicy.h"
#include "../MotionWallpaper.Renderer/FrameTiming.h"
#include "../MotionWallpaper.Renderer/SoftwareFramePolicy.h"
#include "../MotionWallpaper.Renderer/DecodePolicy.h"
#include "../MotionWallpaper.Renderer/FrameScheduler.h"
#include "../MotionWallpaper.Renderer/AdapterPolicy.h"
#include "../MotionWallpaper.Renderer/DesktopHostPolicy.h"
#include "../MotionWallpaper.Renderer/TransitionPolicy.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"
#include "../MotionWallpaper.App/MediaLibrary.h"

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    void require(bool condition, char const* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void settings_round_trip_clears_empty_values(fs::path const& root)
    {
        auto path = root / L"settings.json";
        motion::Settings settings;
        settings.selectedGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        settings.selectedMediaId = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        settings.randomGroupId = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        settings.randomIntervalMinutes = 15;
        settings.startWithWindows = true;
        settings.autoLockEnabled = false;
        settings.autoLockTimeoutSeconds = 600;
        settings.displayOffAfterLockEnabled = true;
        settings.displayOffAfterLockDelaySeconds = 30;
        settings.performanceMode = "power-saver";
        settings.displayMode = "primary";
        settings.displayAssignments.push_back({ "MONITOR\\TEST\\1", settings.selectedGroupId, settings.selectedMediaId });
        motion::save_settings(path, settings);
        {
            std::ifstream input(path, std::ios::binary);
            std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            require(json.find("showScreensaverClock") == std::string::npos,
                "removed screen saver clock setting survived serialization");
        }
        auto populated = motion::load_settings(path);
        if (!populated) throw std::runtime_error("saved settings could not be loaded");
        require(populated->selectedMediaId == settings.selectedMediaId, "non-empty selection did not round-trip");
        require(populated->randomIntervalMinutes == 15 && populated->startWithWindows, "new settings did not round-trip");
        require(!populated->autoLockEnabled && populated->autoLockTimeoutSeconds == 600,
            "automatic-lock settings did not round-trip");
        require(populated->displayOffAfterLockEnabled && populated->displayOffAfterLockDelaySeconds == 30,
            "post-lock display-off settings did not round-trip");
        require(populated->performanceMode == "power-saver", "wallpaper performance mode did not round-trip");
        require(populated->displayMode == "primary", "display mode did not round-trip");
        require(populated->displayAssignments.size() == 1 && populated->displayAssignments.front().displayId == "MONITOR\\TEST\\1",
            "per-display wallpaper assignment did not round-trip");

        settings.selectedGroupId.clear();
        settings.selectedMediaId.clear();
        settings.randomGroupId.clear();
        settings.displayAssignments.clear();
        motion::save_settings(path, settings);
        auto cleared = motion::load_settings(path);
        require(cleared.has_value(), "cleared settings could not be loaded");
        require(cleared->version == motion::settings_schema_version, "settings schema version did not round-trip");
        require(cleared->selectedGroupId.empty(), "selected group survived clearing");
        require(cleared->selectedMediaId.empty(), "selected media survived clearing");
        require(cleared->randomGroupId.empty(), "random group survived clearing");
        require(cleared->displayAssignments.empty(), "display assignment survived clearing");
    }

    void application_data_location_preserves_portable_and_legacy_libraries(fs::path const& root)
    {
        auto applicationRoot = root / L"application";
        auto localRoot = root / L"local";
        fs::create_directories(applicationRoot);
        require(motion::ffmpeg_executable_path(applicationRoot) ==
            applicationRoot / L"Tools" / L"ffmpeg" / L"ffmpeg.exe",
            "installed FFmpeg path was detached from the application directory");
        require(motion::select_application_data_directory(applicationRoot, localRoot) == localRoot / L"MotionWallpaper",
            "fresh installed app did not use LocalAppData");

        std::ofstream(applicationRoot / L"portable.mode") << "portable\n";
        require(motion::select_application_data_directory(applicationRoot, localRoot) == applicationRoot,
            "portable marker did not keep data beside the executable");
        fs::remove(applicationRoot / L"portable.mode");

        fs::create_directories(applicationRoot / L"Config");
        require(motion::select_application_data_directory(applicationRoot, localRoot) == applicationRoot,
            "legacy configuration was detached from its executable");
        fs::remove_all(applicationRoot / L"Config");

        fs::create_directories(applicationRoot / L"Wallpapers");
        require(motion::select_application_data_directory(applicationRoot, localRoot) == applicationRoot,
            "legacy wallpaper library was detached from its executable");
    }

    void legacy_settings_are_migrated(fs::path const& root)
    {
        auto path = root / L"legacy-settings.json";
        std::ofstream(path) << R"({"version":6,"selectedVideoId":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","displayMode":"span","autoLockEnabled":false,"lockTimeoutSeconds":60})";
        auto settings = motion::load_settings(path);
        if (!settings) throw std::runtime_error("legacy settings could not be loaded");
        require(settings->selectedMediaId == "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb", "legacy media key was not loaded");
        require(settings->version == motion::settings_schema_version, "legacy settings were not migrated to the current schema");
        require(settings->displayMode == "independent", "legacy span mode was not migrated to per-display cover mode");
        require(!settings->autoLockEnabled && settings->autoLockTimeoutSeconds == 60,
            "legacy automatic-lock settings were not migrated");
        require(!settings->displayOffAfterLockEnabled,
            "legacy lock-only settings unexpectedly enabled display-off");
        motion::save_settings(path, *settings);
        std::ifstream input(path, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(json.find("selectedMediaId") != std::string::npos, "canonical media key was not saved");
        require(json.find("selectedVideoId") == std::string::npos, "legacy media key survived migration");
        require(json.find("autoLockEnabled") != std::string::npos &&
            json.find("autoLockTimeoutSeconds") != std::string::npos,
            "canonical automatic-lock settings were not saved");
        require(json.find("displayOffAfterLockEnabled") != std::string::npos &&
            json.find("displayOffAfterLockDelaySeconds") != std::string::npos,
            "canonical post-lock display-off settings were not saved");
        require(json.find("lockTimeoutSeconds") == std::string::npos,
            "legacy lock timeout survived migration");
    }

    void coupled_lock_and_display_off_settings_are_migrated(fs::path const& root)
    {
        auto path = root / L"version-7-settings.json";
        std::ofstream(path) << R"({"version":7,"displayOffEnabled":true,"displayOffTimeoutSeconds":600})";
        auto settings = motion::load_settings(path);
        if (!settings) throw std::runtime_error("version 7 settings could not be loaded");
        require(settings->autoLockEnabled && settings->autoLockTimeoutSeconds == 600,
            "coupled lock timeout was not preserved during migration");
        require(settings->displayOffAfterLockEnabled && settings->displayOffAfterLockDelaySeconds == 30,
            "version 7 settings did not receive the post-lock display-off default");
        motion::save_settings(path, *settings);
        std::ifstream input(path, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        require(json.find("displayOffEnabled") == std::string::npos &&
            json.find("displayOffTimeoutSeconds") == std::string::npos,
            "coupled version 7 settings survived migration");
    }

    void unsafe_media_paths_are_rejected(fs::path const& root)
    {
        auto path = root / L"unsafe-media.json";
        std::ofstream(path) << R"({"id":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","groupId":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","name":"unsafe","kind":"video","fileName":"../outside.mp4"})";
        require(!motion::load_media(path).has_value(), "media path escaped its library directory");
        require(motion::safe_file_name(L"source.mp4"), "normal media file name was rejected");
        require(!motion::safe_file_name(L"folder/source.mp4"), "nested media file name was accepted");
        require(!motion::safe_file_name(L"C:\\outside.mp4"), "absolute media file name was accepted");
    }

    void desktop_state_is_deterministic()
    {
        motion::Settings settings;
        require(motion::desktop_intent(settings, false, true) == motion::DesktopIntent::Play, "active desktop should play");
        settings.activePlaybackEnabled = false;
        require(motion::desktop_intent(settings, false, true) == motion::DesktopIntent::Freeze, "inactive playback should freeze");
        settings.continueWhenCovered = false;
        require(motion::desktop_intent(settings, true, true) == motion::DesktopIntent::Pause, "covered desktop should pause");
        settings.desktopPlayback = false;
        require(motion::desktop_intent(settings, false, true) == motion::DesktopIntent::Off, "disabled desktop should stop");
        require(motion::desktop_intent(settings, false, false) == motion::DesktopIntent::Off, "missing media should stop");
    }

    void presentation_state_has_explicit_priorities()
    {
        motion::Settings settings;
        settings.idleTimeoutSeconds = 30;
        using motion::agent::RuntimeAction;
        require(motion::agent::reduce_runtime_action(settings, { false, true, false, true, 60 }) == RuntimeAction::DisplayOff,
            "display power-off must win over session lock");
        require(motion::agent::reduce_runtime_action(settings, { true, true, false, true, 60 }) == RuntimeAction::Locked,
            "session lock must win over playback");
        require(motion::agent::reduce_runtime_action(settings, { true, false, true, true, 30 }) == RuntimeAction::ScreensaverPlay,
            "screen saver must win over coverage");
        require(motion::agent::reduce_runtime_action(settings, { true, false, false, true, 29 }) == RuntimeAction::DesktopPlay,
            "active desktop should play");
        settings.activePlaybackEnabled = false;
        require(motion::agent::reduce_runtime_action(settings, { true, false, false, true, 0 }) == RuntimeAction::DesktopFrozen,
            "inactive desktop should freeze");
        require(motion::agent::reduce_runtime_action(settings, { true, false, false, false, 60 }) == RuntimeAction::Stopped,
            "missing media should stop");
    }

    void identifiers_are_path_safe()
    {
        require(motion::valid_id("01234567-89ab-cdef-0123-456789abcdef"), "valid id rejected");
        require(!motion::valid_id("../escape"), "path traversal id accepted");
        require(!motion::valid_id("UPPERCASE"), "uppercase id accepted");
        require(!motion::valid_id(""), "empty id accepted");
    }

    void future_settings_are_rejected(fs::path const& root)
    {
        auto path = root / L"future-settings.json";
        std::ofstream(path) << R"({"version":999,"desktopPlayback":false})";
        require(!motion::load_settings(path).has_value(), "unsupported future settings schema was accepted");
    }

    void media_activity_suspends_idle_time()
    {
        using namespace std::chrono_literals;
        motion::IdleTimer timer;
        require(timer.Update(300s, 10, 120s, false) == 120s, "initial Windows idle time was not preserved");
        require(timer.Update(310s, 10, 130s, true) == 0s, "media activity did not suspend idle time");
        require(timer.Update(370s, 10, 190s, true) == 0s, "idle time advanced during media activity");
        require(timer.Update(371s, 10, 191s, false) == 1s, "idle time did not restart after media activity");
        require(timer.Update(372s, 11, 0s, false) == 0s, "new user input did not reset idle time");
    }

    void audio_allows_screensaver_but_defers_automatic_lock()
    {
        using namespace std::chrono_literals;
        motion::IdleTimer screensaver;
        motion::IdleTimer automaticLock;
        require(screensaver.Update(60s, 10, 30s, false) == 30s, "audio incorrectly deferred the screen saver");
        require(automaticLock.Update(60s, 10, 30s, true) == 0s, "audio did not defer automatic lock");
    }

    void external_media_remains_authoritative_during_own_screensaver()
    {
        using namespace std::chrono_literals;
        require(motion::agent::screensaver_idle_is_inhibited(true, false),
            "external display playback no longer defers the screen saver");
        require(!motion::agent::screensaver_idle_is_inhibited(true, true),
            "the running screen saver reset its own idle timer");
        require(motion::agent::automatic_lock_idle_is_inhibited(true, true, false),
            "external media playback no longer defers automatic lock");
        require(motion::agent::automatic_lock_idle_is_inhibited(true, true, true),
            "external media inhibition was discarded when the screen saver started");
        require(!motion::agent::automatic_lock_idle_is_inhibited(false, false, true),
            "the application's own screen saver inhibited automatic lock without an external media request");
        motion::IdleTimer automaticLock;
        require(automaticLock.Update(30s, 10, 30s, false) == 30s,
            "lock timer did not preserve idle time when the screen saver started");
        require(automaticLock.Update(60s, 10, 60s,
            motion::agent::automatic_lock_idle_is_inhibited(false, false, true)) == 60s,
            "automatic lock did not advance during the application's own screen saver");
        require(automaticLock.Update(61s, 10, 61s,
            motion::agent::automatic_lock_idle_is_inhibited(true, true, true)) == 0s,
            "external playback did not reset automatic-lock idle while the screen saver was active");
    }

    void display_off_waits_for_the_post_lock_delay()
    {
        using namespace std::chrono_literals;
        using motion::agent::display_off_after_lock_is_due;
        require(!display_off_after_lock_is_due(true, 29s, 30, false, true),
            "display powered off before the post-lock delay");
        require(display_off_after_lock_is_due(true, 30s, 30, false, true),
            "display did not power off when the post-lock delay elapsed");
        require(!display_off_after_lock_is_due(false, 30s, 30, false, true),
            "disabled post-lock display-off still fired");
        require(!display_off_after_lock_is_due(true, 30s, 30, true, true),
            "post-lock display-off fired more than once");
        require(!display_off_after_lock_is_due(true, 30s, 30, false, false),
            "post-lock display-off ignored retry backoff");
    }

    void fullscreen_coverage_is_not_limited_to_foreground()
    {
        motion::agent::WindowBounds display{ 0, 0, 1920, 1080 };
        motion::agent::WindowBounds focusedSmall{ 200, 200, 900, 700 };
        motion::agent::WindowBounds backgroundFullscreen{ 0, 0, 1920, 1080 };
        require(!motion::agent::covers_display(focusedSmall, display), "small focused window was treated as fullscreen");
        require(motion::agent::covers_display(backgroundFullscreen, display),
            "non-focused fullscreen window no longer covers the desktop");
    }

    void normal_pause_keeps_decoder_hot()
    {
        require(!motion::renderer::should_compact_idle(false),
            "normal pause still destroys the decoder and causes a resume GPU spike");
        require(motion::renderer::residency_timer_delay_ms(false) == 30'000,
            "idle memory pressure checks became too frequent");
        require(motion::renderer::should_compact_idle(true) &&
            motion::renderer::residency_timer_delay_ms(true) == 250,
            "low-memory pause no longer releases decoder resources promptly");
    }

    void stable_agent_states_do_not_poll_at_twenty_hertz()
    {
        require(motion::agent::runtime_wait_interval_ms(false) == 50,
            "pending Renderer transitions lost their responsive ACK retry");
        require(motion::agent::runtime_wait_interval_ms(true) == 1000,
            "stable playback still wakes the Agent policy loop at high frequency");
        require(motion::agent::runtime_wait_interval_ms(true, true) == 50,
            "short media-library transactions lost their responsive hold interval");
    }

    void battery_power_pauses_optional_variant_generation()
    {
        require(!motion::agent::variant_generation_allowed(true, true, true),
            "battery power still allows background video transcoding");
        require(motion::agent::variant_generation_allowed(false, true, false),
            "an AC-powered priority performance-copy request was blocked");
        require(motion::agent::variant_generation_allowed(false, false, true),
            "AC-powered idle time no longer permits deferred performance-copy work");
        require(!motion::agent::variant_generation_allowed(false, false, false),
            "non-priority transcoding competes with active playback");
    }

    void playback_capability_only_degrades_software_devices()
    {
        require(!motion::agent::uses_software_playback("auto", true),
            "automatic playback downgraded a physical video device");
        require(motion::agent::uses_software_playback("auto", false) &&
            motion::agent::uses_software_playback("software", true),
            "WARP-only or explicitly software playback missed the CPU profile");
        require(!motion::agent::uses_software_playback("hardware", false),
            "strict hardware mode silently changed into software playback");

        auto strong = motion::agent::software_playback_profile(true, 16, 2560, 1600, 165);
        require(strong.enabled && strong.width == 1728 && strong.height == 1080 && strong.frameRate == 60,
            "a strong CPU did not receive the bounded 1080p60 smoothness profile");
        auto medium = motion::agent::software_playback_profile(true, 8, 2560, 1440, 144);
        require(medium.width == 1280 && medium.height == 720 && medium.frameRate == 60,
            "a mid-range CPU did not receive the 720p60 smoothness profile");
        auto modest = motion::agent::software_playback_profile(true, 4, 2560, 1440, 144);
        require(modest.width == 1280 && modest.height == 720 && modest.frameRate == 30,
            "a modest CPU was assigned more than the 720p30 safety budget");
        auto weak = motion::agent::software_playback_profile(true, 2, 2560, 1440, 60);
        require(weak.width == 854 && weak.height == 480 && weak.frameRate == 30,
            "a weak CPU was assigned more than the 480p30 safety budget");
        require(!motion::agent::software_playback_profile(false, 2, 7680, 4320, 240).enabled,
            "physical-GPU playback was unexpectedly constrained by CPU tiering");
    }

    void software_presentation_governor_recovers_without_catchup_bursts()
    {
        require(motion::renderer::software_probe_interval_ms(60) == 17 &&
            motion::renderer::software_probe_interval_ms(30) == 33,
            "software frame pacing no longer has bounded 60/30 FPS waits");
        motion::renderer::SoftwareFrameGovernor governor;
        governor.Configure(60);
        for (int index = 0; index < 7; ++index) {
            require(!governor.Observe(14'000), "software frame cap reacted to a single transient too early");
        }
        require(governor.Observe(14'000) && governor.ActiveFrameRate() == 30,
            "repeated WARP deadline misses did not reduce presentation pressure");
        for (int index = 0; index < 299; ++index) {
            require(!governor.Observe(5'000), "software frame cap recovered before ten stable seconds");
        }
        require(governor.Observe(5'000) && governor.ActiveFrameRate() == 60,
            "software frame cap did not recover after sustained headroom");
    }

    void screensaver_pause_returns_window_to_desktop()
    {
        using motion::renderer::Command;
        using motion::renderer::PresentationMode;
        require(motion::renderer::leaves_screensaver(PresentationMode::Screensaver, Command::Pause),
            "screen saver pause left its topmost window covering the desktop");
        require(motion::renderer::leaves_screensaver(PresentationMode::Screensaver, Command::DesktopPlay),
            "screen saver play-to-desktop transition did not restore the desktop host");
        require(!motion::renderer::leaves_screensaver(PresentationMode::Screensaver, Command::ScreensaverPlay),
            "continuing screen saver playback unexpectedly requested desktop reparenting");
        require(!motion::renderer::leaves_screensaver(PresentationMode::Desktop, Command::Pause),
            "ordinary desktop pause unexpectedly requested reparenting");
    }

    void desktop_host_must_cover_the_virtual_screen()
    {
        using motion::renderer::usable_desktop_host_bounds;
        require(usable_desktop_host_bounds(true, 0, 0, 2560, 1440, 0, 0, 2560, 1440),
            "full-size Progman desktop host was rejected");
        require(!usable_desktop_host_bounds(false, 0, 0, 2560, 1440, 0, 0, 2560, 1440),
            "hidden WorkerW desktop host was accepted");
        require(!usable_desktop_host_bounds(true, 0, 0, 136, 39, 0, 0, 2560, 1440),
            "tiny WorkerW desktop host was accepted");
        require(usable_desktop_host_bounds(true, -1920, 0, 2560, 1440, -1920, 0, 2560, 1440),
            "multi-monitor virtual desktop host was rejected");
    }

    void manual_selection_wins_over_group_randomization()
    {
        using motion::agent::RandomSelectionAction;
        using motion::agent::random_selection_action;
        require(random_selection_action(true, false, true, false, false, true) == RandomSelectionAction::UseSelected,
            "manual wallpaper selection did not override the current random item");
        require(random_selection_action(true, false, true, true, true, true) == RandomSelectionAction::UseSelected,
            "explicit user selection lost to a simultaneous automatic trigger");
        require(random_selection_action(true, true, true, false, false, false) == RandomSelectionAction::ChooseRandom,
            "enabling a random group did not choose its initial item");
        require(random_selection_action(false, false, false, true, true, true) == RandomSelectionAction::UseSelected,
            "inactive random group overrode the selected wallpaper");
        require(random_selection_action(true, false, false, false, false, true) == RandomSelectionAction::KeepRandom,
            "stable random selection changed without a trigger");
    }

    void identical_media_share_one_renderer()
    {
        auto shared = motion::agent::renderer_media_key(L"C:\\wallpapers\\valley.mp4", "video");
        auto other = motion::agent::renderer_media_key(L"C:\\wallpapers\\beach.mp4", "video");
        std::vector<motion::agent::RendererRoute> routes{
            { shared, L"\\\\.\\DISPLAY1" },
            { shared, L"\\\\.\\DISPLAY2" }
        };
        auto grouped = motion::agent::group_renderer_routes(routes, true);
        require(grouped.size() == 1 && grouped.front().monitorDevices.size() == 2,
            "the same wallpaper no longer shares one Renderer across displays");

        routes.push_back({ other, L"\\\\.\\DISPLAY3" });
        grouped = motion::agent::group_renderer_routes(routes, true);
        require(grouped.size() == 2, "different wallpapers were incorrectly forced through one Renderer");

        routes.push_back({ shared, L"\\\\.\\DISPLAY4", L"other-adapter" });
        grouped = motion::agent::group_renderer_routes(routes, true);
        require(grouped.size() == 3,
            "the same wallpaper was incorrectly shared across display adapters");

        auto firstUnknown = motion::agent::renderer_adapter_key({}, L"\\\\.\\DISPLAY5");
        auto secondUnknown = motion::agent::renderer_adapter_key({}, L"\\\\.\\DISPLAY6");
        require(!firstUnknown.empty() && firstUnknown != secondUnknown,
            "unknown indirect-display adapters collapse into one Renderer route");

        grouped = motion::agent::group_renderer_routes({ { shared, {} } }, false);
        require(grouped.size() == 1 && grouped.front().monitorDevices.empty(),
            "primary-only rendering unexpectedly retained a monitor route");
    }

    void video_variant_policy_preserves_quality_priority()
    {
        using motion::agent::VideoSourceCodec;
        require(motion::agent::video_software_fallback_allowed(VideoSourceCodec::Hevc, true, 1),
            "8-bit HEVC Main unexpectedly lost the bounded software fallback");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::Hevc, true, 2),
            "HEVC Main10 was allowed to fall through to an 8-bit software encoder");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::Hevc, false, 0),
            "unknown HEVC bit depth was treated as safe for 8-bit software encoding");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::H264, true, 100, true),
            "HDR transfer metadata was ignored by the software fallback guard");
        require(!motion::agent::video_software_fallback_allowed(VideoSourceCodec::Unknown, false, 0, false, true),
            "BT.2020 primaries were ignored by the software fallback guard");
        require(motion::agent::video_cpu_conversion_allowed(false, false) &&
            !motion::agent::video_cpu_conversion_allowed(true, false) &&
            !motion::agent::video_cpu_conversion_allowed(false, true),
            "CPU compatibility copies no longer distinguish SDR Main10 from HDR/BT.2020");
        auto original = motion::agent::video_variant_decision("original");
        require(!original.targetFps && original.fileName.empty(), "original mode unexpectedly requested a proxy");
        auto balanced = motion::agent::video_variant_decision("balanced", 2560, 1440, 240, 1, 165);
        require(balanced.targetFps == 120 && balanced.fileName == L"balanced-120-2560x1440-v4.mp4",
            "balanced mode no longer targets the high-quality 120 FPS proxy");
        auto sixtyHertz = motion::agent::video_variant_decision("balanced", 2560, 1440, 240, 1, 60);
        require(sixtyHertz.targetFps == 60 && sixtyHertz.fileName == L"balanced-60-2560x1440-v4.mp4",
            "balanced mode generated frames the display cannot present");
        auto powerSaver = motion::agent::video_variant_decision("power-saver", 2560, 1440, 240, 1, 165);
        require(powerSaver.targetFps == 60 && powerSaver.fileName == L"power-saver-60-2560x1440-v4.mp4",
            "power saver did not retain its explicit 60 FPS policy");
        auto cpuSmooth = motion::agent::video_variant_decision("cpu-smooth", 1280, 720, 240, 1, 60);
        require(cpuSmooth.targetFps == 60 && cpuSmooth.fileName == L"cpu-smooth-60-1280x720-v5.mp4",
            "software playback did not receive its isolated CPU-friendly cache identity");
        auto nativeRate = motion::agent::video_variant_decision("balanced", 2560, 1440, 30, 1, 165);
        require(nativeRate.targetFps == 30, "balanced mode inserted frames missing from the source");
        require(motion::agent::video_variant_rate_matches(60'000, 1'001, 60),
            "59.94 FPS container rate was incorrectly rejected as non-60 FPS");
        require(!motion::agent::video_variant_rate_matches(120, 1, 60),
            "a different performance tier passed the variant frame-rate check");
        require(motion::agent::video_variant_dimensions_match(3'840, 2'176, 3'840, 2'160),
            "valid HEVC coding-block padding was rejected");
        require(!motion::agent::video_variant_dimensions_match(1'920, 1'080, 3'840, 2'160),
            "a different visible resolution passed variant validation");
        require(motion::agent::video_needs_variant(240, 1, 120), "240 FPS source was not optimized");
        require(!motion::agent::video_needs_variant(120, 1, 120), "120 FPS source was unnecessarily transcoded");
        require(motion::agent::video_needs_variant(60, 1, 60, 3840, 2160, 2560, 1440),
            "display-resolution optimization was skipped when frame rate already matched");
        auto fitted = motion::agent::video_variant_dimensions(3'840, 2'160, 2'560, 1'600);
        require(fitted.first == 2'846 && fitted.second == 1'600,
            "display-aware variant no longer preserves cover resolution");
        auto native = motion::agent::video_variant_dimensions(3'840, 2'160, 3'840, 2'160);
        require(native.first == 3'840 && native.second == 2'160,
            "display-aware variant unexpectedly upscaled or cropped a native 4K target");
        auto cpuWide = motion::agent::video_cpu_variant_dimensions(3'840, 720, 1'920, 1'080);
        require(cpuWide.first == 1'920 && cpuWide.second == 360,
            "ultrawide CPU playback escaped the hard software pixel budget");
        auto cpuPortrait = motion::agent::video_cpu_variant_dimensions(2'160, 3'840, 1'080, 1'920);
        require(cpuPortrait.first == 1'080 && cpuPortrait.second == 1'920,
            "portrait CPU playback no longer preserves source aspect ratio");
    }

    void variant_requests_use_last_writer_wins(fs::path const& root)
    {
        auto mediaDirectory = root / L"variant-request-order";
        fs::create_directories(mediaDirectory / L"Variants");

        require(motion::request_variant_generation(mediaDirectory, "balanced"),
            "balanced request could not be persisted");
        require(motion::request_variant_generation(mediaDirectory, "power-saver"),
            "newer power-saver request could not replace balanced");
        require(motion::pause_variant_generation(mediaDirectory) &&
            motion::inspect_variant_cache(mediaDirectory).paused,
            "a queued optimization request could not be paused");
        require(motion::resume_variant_generation(mediaDirectory) &&
            !motion::inspect_variant_cache(mediaDirectory).paused,
            "a paused optimization request could not be resumed");
        motion::complete_variant_generation(mediaDirectory, "balanced");
        require(motion::read_variant_request(mediaDirectory) == "power-saver",
            "an obsolete completion cleared the newer request");
        motion::fail_variant_generation(mediaDirectory, "balanced");
        require(motion::read_variant_request(mediaDirectory) == "power-saver" &&
            !fs::exists(motion::variant_failed_path(mediaDirectory)),
            "an obsolete failure replaced the newer request");

        std::ofstream(mediaDirectory / L"Variants" / L"balanced-120-2560x1440-v4.mp4",
            std::ios::binary) << "balanced";
        std::ofstream(mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v2.mp4",
            std::ios::binary) << "legacy";
        auto status = motion::inspect_variant_cache(mediaDirectory);
        require(status.requestedMode == "power-saver" && status.entries.size() == 2,
            "variant status lost the active request or cached files");
        require(motion::select_variant_file(status, "balanced").starts_with(L"balanced-") &&
            motion::select_variant_file(status, "original").starts_with(L"balanced-") &&
            motion::select_variant_file(status, "power-saver").starts_with(L"power-saver-"),
            "retained variant selection no longer honors profile priority and original fallback");
        auto balanced = std::find_if(status.entries.begin(), status.entries.end(), [](auto const& entry) {
            return entry.mode == "balanced";
        });
        auto powerSaver = std::find_if(status.entries.begin(), status.entries.end(), [](auto const& entry) {
            return entry.mode == "power-saver";
        });
        require(balanced != status.entries.end() && powerSaver != status.entries.end(),
            "performance cache files were assigned to the wrong profile");

        auto currentPowerSaver = mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v4.mp4";
        std::ofstream(currentPowerSaver, std::ios::binary) << "current";
        require(!motion::retain_variant_profile(mediaDirectory, "power-saver", L"missing.mp4") &&
            fs::is_regular_file(mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v2.mp4"),
            "profile retention removed a cache before validating its keep target");
        require(motion::retain_variant_profile(mediaDirectory, "power-saver",
            currentPowerSaver.filename().wstring()) &&
            !fs::exists(mediaDirectory / L"Variants" / L"power-saver-60-2560x1440-v2.mp4") &&
            fs::is_regular_file(currentPowerSaver) &&
            fs::is_regular_file(mediaDirectory / L"Variants" / L"balanced-120-2560x1440-v4.mp4"),
            "profile retention did not remove only the superseded same-tier copy");

        auto sharedDirectory = root / L"variant-shared-storage";
        fs::create_directories(sharedDirectory / L"Variants");
        auto sharedBalanced = sharedDirectory / L"Variants" / L"balanced-60-2560x1440-v4.mp4";
        auto sharedPowerSaver = sharedDirectory / L"Variants" / L"power-saver-60-2560x1440-v4.mp4";
        std::ofstream(sharedBalanced, std::ios::binary) << "shared-copy";
        fs::create_hard_link(sharedBalanced, sharedPowerSaver);
        auto sharedStatus = motion::inspect_variant_cache(sharedDirectory);
        require(sharedStatus.entries.size() == 2 && sharedStatus.files == 1 &&
            sharedStatus.bytes == fs::file_size(sharedBalanced) &&
            sharedStatus.entries[0].sharedStorage && sharedStatus.entries[1].sharedStorage,
            "hard-linked performance profiles were counted as duplicate physical storage");

        motion::fail_variant_generation(mediaDirectory, "power-saver");
        auto failedStatus = motion::inspect_variant_cache(mediaDirectory);
        require(failedStatus.failed && failedStatus.failedMode == "power-saver" &&
            failedStatus.requestedMode.empty(),
            "a failed performance-copy task did not retain its retryable profile identity");
        require(motion::request_variant_generation(mediaDirectory, "power-saver"),
            "a failed performance-copy task could not be retried");
        motion::complete_variant_generation(mediaDirectory, "power-saver");
        require(motion::read_variant_request(mediaDirectory).empty(),
            "the matching completion did not clear its request");

        require(motion::suppress_variant_generation(mediaDirectory, "power-saver"),
            "a deleted profile could not persist its suppression marker");
        require(motion::variant_generation_suppressed(mediaDirectory, "power-saver"),
            "a deleted profile would be regenerated automatically");
        require(motion::request_variant_generation(mediaDirectory, "power-saver") &&
            !motion::variant_generation_suppressed(mediaDirectory, "power-saver"),
            "manual generation did not re-enable a deleted profile");
    }

    void video_transcoder_fails_closed_without_backend(fs::path const& root)
    {
        std::wstring error;
        auto result = motion::agent::transcode_video(
            root / L"missing-ffmpeg.exe", root / L"source.mp4", root / L"output.mp4",
            3840, 2160, 60, [] { return motion::agent::VideoTranscodeControl::running; }, error);
        require(result == motion::agent::VideoTranscodeResult::unsupported && !error.empty(),
            "missing optimization backend did not safely fall back to the source video");
    }

    void video_transcoder_orders_vendor_backends_and_bounds_software_fallback()
    {
        using motion::agent::VideoTranscodeAdapter;
        using motion::agent::VideoTranscodeBackend;
        auto hybrid = motion::agent::video_transcode_backend_order({
            VideoTranscodeAdapter{ 0x8086, 512ULL * 1024 * 1024 },
            VideoTranscodeAdapter{ 0x10de, 8ULL * 1024 * 1024 * 1024 }
        }, 2560, 1440, 120);
        require(hybrid.size() == 3 &&
            hybrid[0] == VideoTranscodeBackend::nvidiaCudaNvenc &&
            hybrid[1] == VideoTranscodeBackend::nvidiaNvenc &&
            hybrid[2] == VideoTranscodeBackend::intelQsv,
            "hybrid GPU transcode order did not prefer the discrete adapter or bounded software work");

        auto amd = motion::agent::video_transcode_backend_order({
            VideoTranscodeAdapter{ 0x1002, 4ULL * 1024 * 1024 * 1024 }
        }, 2560, 1440, 60);
        require(amd.size() == 2 && amd[0] == VideoTranscodeBackend::amdAmf &&
            amd[1] == VideoTranscodeBackend::softwareKvazaar,
            "AMD hardware encoding did not retain a safe software fallback");

        auto cpuOnly = motion::agent::video_transcode_backend_order({}, 1920, 1080, 60);
        require(cpuOnly.size() == 1 && cpuOnly[0] == VideoTranscodeBackend::softwareKvazaar,
            "CPU-only systems lost their bounded software encoder fallback");
        require(motion::agent::video_transcode_backend_order({}, 1920, 1080, 60, true, false).empty(),
            "an HDR or high-bit-depth source was allowed through the 8-bit software encoder");
        require(motion::agent::video_transcode_backend_order({}, 3840, 2160, 60).empty(),
            "unsafe 4K software encoding was scheduled on a CPU-only system");

        auto cpuPlayback = motion::agent::video_transcode_backend_order(
            {}, 1920, 1080, 60, true, true, true);
        require(cpuPlayback.size() == 1 && cpuPlayback[0] == VideoTranscodeBackend::softwareOpenH264,
            "CPU playback copy did not force the broadly decodable H.264 encoder");
        require(motion::agent::video_transcode_backend_order(
            {}, 1920, 1080, 60, true, false, true).empty(),
            "HDR or high-bit-depth video was destructively converted for CPU playback");

        auto unknownProbe = motion::agent::video_transcode_backend_order({}, 2560, 1440, 120, false);
        require(unknownProbe.size() == 4 &&
            unknownProbe[0] == VideoTranscodeBackend::nvidiaCudaNvenc &&
            unknownProbe[1] == VideoTranscodeBackend::nvidiaNvenc &&
            unknownProbe[2] == VideoTranscodeBackend::intelQsv &&
            unknownProbe[3] == VideoTranscodeBackend::amdAmf,
            "a failed adapter probe did not preserve hardware encoder discovery by execution");
    }

    struct FrameSchedulerProbe
    {
        static constexpr UINT message = WM_APP + 77;
        motion::renderer::FrameScheduler scheduler{ message };
        std::vector<std::chrono::steady_clock::time_point> ticks;

        static LRESULT CALLBACK WindowProc(HWND window, UINT event, WPARAM wParam, LPARAM lParam)
        {
            auto self = reinterpret_cast<FrameSchedulerProbe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (event == WM_NCCREATE) {
                auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                self = static_cast<FrameSchedulerProbe*>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (self && event == message) {
                self->scheduler.TickHandled();
                self->ticks.push_back(std::chrono::steady_clock::now());
                // Approximate non-trivial frame transfer/presentation work.
                Sleep(3);
                if (self->ticks.size() < 30) self->scheduler.Start(window, 8);
                return 0;
            }
            return DefWindowProcW(window, event, wParam, lParam);
        }
    };

    void frame_scheduler_uses_real_interval()
    {
        require(motion::renderer::frame_due_time_100ns(25) == -250'000, "frame interval was not converted to a real deadline");
        require(motion::renderer::presentation_probe_interval_ms(333'333) == 24,
            "30 fps playback did not retain a pre-frame retry window");
        require(motion::renderer::presentation_probe_interval_ms(166'667) == 13,
            "60 fps playback wake-up is outside its low-power window");
        require(motion::renderer::presentation_probe_interval_ms(83'333) == 6,
            "high-frame-rate playback wake-up is too late");
        require(motion::renderer::presentation_probe_interval_ms(666'667) == 24,
            "a skipped frame escaped the bounded scheduling interval");
        motion::unique_handle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
        if (!timer) timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
        require(static_cast<bool>(timer), "waitable timer could not be created");
        LARGE_INTEGER due{};
        due.QuadPart = motion::renderer::frame_due_time_100ns(25);
        auto started = std::chrono::steady_clock::now();
        require(SetWaitableTimerEx(timer.get(), &due, 0, nullptr, nullptr, nullptr, 0) != FALSE, "frame timer could not be armed");
        require(WaitForSingleObject(timer.get(), 250) == WAIT_OBJECT_0, "frame timer did not fire");
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        require(elapsed >= std::chrono::milliseconds(18), "frame timer still fired at the old 1 ms cadence");
        require(elapsed < std::chrono::milliseconds(150), "frame timer deadline was excessively late");

        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = FrameSchedulerProbe::WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"MotionWallpaper.Tests.FrameScheduler";
        require(RegisterClassW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "frame scheduler probe window class failed");
        FrameSchedulerProbe probe;
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, windowClass.hInstance, &probe);
        require(window != nullptr, "frame scheduler probe window failed");
        std::atomic_uint64_t pressure{};
        std::jthread load([&](std::stop_token stop) {
            while (!stop.stop_requested()) pressure.fetch_add(1, std::memory_order_relaxed);
        });
        probe.scheduler.Start(window, 8);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (probe.ticks.size() < 30 && std::chrono::steady_clock::now() < deadline) {
            MSG pending{};
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            Sleep(1);
        }
        probe.scheduler.Stop();
        load.request_stop();
        if (window) DestroyWindow(window);
        require(probe.ticks.size() == 30, "real FrameScheduler stalled under processing pressure");
        std::vector<int64_t> intervals;
        for (size_t index = 1; index < probe.ticks.size(); ++index) {
            intervals.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                probe.ticks[index] - probe.ticks[index - 1]).count());
        }
        std::sort(intervals.begin(), intervals.end());
        require(intervals[intervals.size() / 2] >= 5 && intervals[intervals.size() / 2] <= 35,
            "real FrameScheduler median interval drifted outside its workload budget");
        require(intervals.back() < 100, "real FrameScheduler produced a visible long-frame stall");
    }

    void adapter_policy_preserves_heavy_video_throughput()
    {
        require(!motion::renderer::prefer_high_performance_adapter(1920, 1080, 60, 1),
            "ordinary 1080p video unnecessarily wakes the high-performance GPU");
        require(!motion::renderer::prefer_high_performance_adapter(3840, 2160, 30, 1),
            "ordinary 4K30 video unnecessarily wakes the high-performance GPU");
        require(motion::renderer::prefer_high_performance_adapter(3840, 2160, 60, 1),
            "4K60 video no longer receives the throughput-first adapter policy");
        require(motion::renderer::prefer_high_performance_adapter(3840, 2160, 240'000, 1'001),
            "high-frame-rate 4K video was assigned to the power-saving adapter");
        require(!motion::renderer::prefer_high_performance_adapter(0, 2160, 60, 1),
            "invalid media metadata selected the high-performance adapter");
    }

    void renderer_ack_channels_are_isolated()
    {
        auto target = motion::protocol::parse_ack("ack target 41 playing");
        auto control = motion::protocol::parse_ack("ack control 42 stopping");
        require(target.channel == motion::protocol::AckChannel::Target && target.revision == 41, "target ACK was not parsed");
        require(control.channel == motion::protocol::AckChannel::Control && control.revision == 42, "control ACK was not parsed");
        require(control.channel != motion::protocol::AckChannel::Target, "control ACK satisfied a target transition");
        require(motion::protocol::parse_ack("ack 43 playing").channel == motion::protocol::AckChannel::Unknown,
            "legacy ambiguous ACK was accepted");
        auto decode = motion::protocol::parse_decode_status(
            "status decode software-fallback fallback-no-hardware-decoder");
        require(decode.path == "software-fallback" && decode.reason == "fallback-no-hardware-decoder",
            "renderer decode status was not parsed");
        auto automatic = motion::protocol::parse_decode_status(
            "status decode automatic dxgi-manager-enabled");
        require(automatic.path == "automatic" && automatic.reason == "dxgi-manager-enabled",
            "automatic DXGI decode status was not parsed");
        require(motion::protocol::parse_decode_status("status decode hardware").path.empty(),
            "incomplete renderer decode status was accepted");
    }

    void decode_modes_have_distinct_fallback_contracts()
    {
        using motion::renderer::DecodePath;
        require(motion::renderer::select_decode_path(L"auto") == DecodePath::Automatic,
            "automatic decode no longer delegates selection to the DXGI-backed media engine");
        require(motion::renderer::select_decode_path(L"hardware") == DecodePath::Hardware,
            "explicit hardware decode no longer selects the physical GPU path");
        require(motion::renderer::select_decode_path(L"software") == DecodePath::Software,
            "explicit software decode no longer selects the WARP path");
        require(motion::renderer::allows_software_device_fallback(DecodePath::Automatic) &&
            !motion::renderer::allows_software_device_fallback(DecodePath::Hardware) &&
            !motion::renderer::allows_software_device_fallback(DecodePath::Software),
            "software-device fallback is no longer restricted to automatic decode");
    }

    std::string read_protocol_line(HANDLE pipe, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string pending;
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available{};
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available) {
                char character{};
                DWORD read{};
                require(ReadFile(pipe, &character, 1, &read, nullptr) != FALSE,
                    "renderer protocol pipe failed");
                if (read && character == '\n') return pending;
                if (read) pending.push_back(character);
            } else {
                Sleep(10);
            }
        }
        return pending;
    }

    motion::protocol::Ack read_typed_ack(HANDLE pipe, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            auto ack = motion::protocol::parse_ack(read_protocol_line(pipe, remaining));
            if (ack.channel != motion::protocol::AckChannel::Unknown) return ack;
        }
        return {};
    }

    void write_test_bitmap(fs::path const& path)
    {
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + 16;
        info.biSize = sizeof(info);
        info.biWidth = 2;
        info.biHeight = 2;
        info.biPlanes = 1;
        info.biBitCount = 24;
        info.biCompression = BI_RGB;
        info.biSizeImage = 16;
        std::array<unsigned char, 16> pixels{ 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 0, 0, 0 };
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<char const*>(&file), sizeof(file));
        output.write(reinterpret_cast<char const*>(&info), sizeof(info));
        output.write(reinterpret_cast<char const*>(pixels.data()), pixels.size());
    }

    struct RendererProcess
    {
        motion::unique_handle process;
        motion::unique_handle input;
        motion::unique_handle output;

        void Send(std::string const& value) const
        {
            DWORD written{};
            require(WriteFile(input.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr) != FALSE &&
                written == value.size(), "renderer command write failed");
        }
    };

    RendererProcess launch_hidden_image_renderer(fs::path const& root, std::wstring const& name)
    {
        auto renderer = motion::executable_directory().parent_path() / L"MotionWallpaper.App" / L"motionwallpaper-renderer.exe";
        require(fs::is_regular_file(renderer), "renderer executable was not built before integration tests");
        auto image = root / name;
        write_test_bitmap(image);

        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        HANDLE inputReadRaw{}, inputWriteRaw{}, outputReadRaw{}, outputWriteRaw{};
        require(CreatePipe(&inputReadRaw, &inputWriteRaw, &security, 0) != FALSE, "renderer input pipe failed");
        motion::unique_handle inputRead(inputReadRaw), inputWrite(inputWriteRaw);
        require(CreatePipe(&outputReadRaw, &outputWriteRaw, &security, 0) != FALSE, "renderer output pipe failed");
        motion::unique_handle outputRead(outputReadRaw), outputWrite(outputWriteRaw);
        SetHandleInformation(inputWrite.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outputRead.get(), HANDLE_FLAG_INHERIT, 0);

        auto command = motion::build_command_line({ renderer.wstring(), L"-hidden", L"-protocol-test", L"-video", image.wstring(),
            L"-kind", L"image", L"-decode", L"auto", L"-display", L"primary" });
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = inputRead.get();
        startup.hStdOutput = outputWrite.get();
        startup.hStdError = outputWrite.get();
        PROCESS_INFORMATION created{};
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, renderer.parent_path().c_str(), &startup, &created) != FALSE, "renderer integration process failed to launch");
        motion::unique_handle process(created.hProcess), processThread(created.hThread);
        inputRead.reset();
        outputWrite.reset();
        return { std::move(process), std::move(inputWrite), std::move(outputRead) };
    }

    void renderer_process_uses_typed_acks(fs::path const& root)
    {
        auto renderer = launch_hidden_image_renderer(root, L"protocol.bmp");
        renderer.Send("desktop-play 1\n");
        auto target = read_typed_ack(renderer.output.get(), std::chrono::seconds(5));
        require(target.channel == motion::protocol::AckChannel::Target && target.revision == 1, "renderer did not emit a typed target ACK");
        renderer.Send("legacy-play 8\n");
        auto error = read_protocol_line(renderer.output.get(), std::chrono::seconds(2));
        require(error.starts_with("error 8 invalid-command"), "renderer accepted a removed protocol alias");
        renderer.Send("stop 2\n");
        auto control = read_typed_ack(renderer.output.get(), std::chrono::seconds(2));
        require(control.channel == motion::protocol::AckChannel::Control && control.revision == 2, "renderer did not emit a typed control ACK");
        require(WaitForSingleObject(renderer.process.get(), 3000) == WAIT_OBJECT_0, "renderer did not stop after the stop command");
    }

    void renderer_exits_when_agent_pipe_closes(fs::path const& root)
    {
        auto renderer = launch_hidden_image_renderer(root, L"protocol-eof.bmp");
        renderer.Send("desktop-play 11\n");
        auto target = read_typed_ack(renderer.output.get(), std::chrono::seconds(5));
        require(target.channel == motion::protocol::AckChannel::Target && target.revision == 11,
            "renderer was not ready before the EOF test");
        renderer.input.reset();
        require(WaitForSingleObject(renderer.process.get(), 3000) == WAIT_OBJECT_0,
            "renderer survived after its Agent command pipe closed");
    }

    motion::unique_handle launch_writer_process(std::wstring_view mode, fs::path const& path)
    {
        auto executable = motion::executable_directory() / L"MotionWallpaper.Tests.exe";
        std::wstring command = L"\"" + executable.wstring() + L"\" " + std::wstring(mode) + L" \"" + path.wstring() + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION created{};
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, executable.parent_path().c_str(), &startup, &created) != FALSE,
            "configuration writer process failed to launch");
        CloseHandle(created.hThread);
        return motion::unique_handle(created.hProcess);
    }

    void settings_and_runtime_have_single_writers(fs::path const& root)
    {
        auto settingsPath = root / L"single-writer" / L"settings.json";
        auto runtimePath = root / L"single-writer" / L"runtime.json";
        auto settingsWriter = launch_writer_process(L"--write-settings", settingsPath);
        auto runtimeWriter = launch_writer_process(L"--write-runtime", runtimePath);
        HANDLE writers[]{ settingsWriter.get(), runtimeWriter.get() };
        require(WaitForMultipleObjects(2, writers, TRUE, 10'000) == WAIT_OBJECT_0,
            "configuration writer processes timed out");
        DWORD settingsExit{}, runtimeExit{};
        require(GetExitCodeProcess(settingsWriter.get(), &settingsExit) && settingsExit == 0,
            "settings writer process failed");
        require(GetExitCodeProcess(runtimeWriter.get(), &runtimeExit) && runtimeExit == 0,
            "runtime writer process failed");

        auto loadedSettings = motion::load_settings(settingsPath);
        auto loadedRuntime = motion::load_runtime(runtimePath);
        require(loadedSettings && loadedSettings->idleTimeoutSeconds == 777 &&
            loadedSettings->selectedMediaId == "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
            "runtime publication overwrote user settings");
        require(loadedRuntime && loadedRuntime->activeMediaId == "cccccccc-cccc-cccc-cccc-cccccccccccc" &&
            loadedRuntime->decodePath == "software-fallback" &&
            loadedRuntime->decodeReason == "fallback-no-hardware-decoder",
            "settings publication overwrote runtime selection");
    }

    void automatic_decode_runtime_round_trips(fs::path const& root)
    {
        auto path = root / L"automatic-runtime.json";
        motion::RuntimeState runtime;
        runtime.activeGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        runtime.activeMediaId = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        runtime.decodePath = "automatic";
        runtime.decodeReason = "dxgi-manager-enabled";
        motion::save_runtime(path, runtime);

        auto loaded = motion::load_runtime(path);
        require(loaded && loaded->decodePath == "automatic" &&
            loaded->decodeReason == "dxgi-manager-enabled",
            "automatic DXGI decode status did not survive runtime publication");
    }

    void concurrent_settings_writers_never_publish_torn_json(fs::path const& root)
    {
        auto path = root / L"concurrent-settings" / L"settings.json";
        auto first = launch_writer_process(L"--write-settings-a", path);
        auto second = launch_writer_process(L"--write-settings-b", path);
        HANDLE writers[]{ first.get(), second.get() };
        require(WaitForMultipleObjects(2, writers, TRUE, 10'000) == WAIT_OBJECT_0,
            "concurrent settings writers timed out");
        DWORD firstExit{}, secondExit{};
        if (!GetExitCodeProcess(first.get(), &firstExit) || !GetExitCodeProcess(second.get(), &secondExit) ||
            firstExit != 0 || secondExit != 0) {
            throw std::runtime_error("a concurrent settings writer failed (" +
                std::to_string(firstExit) + ", " + std::to_string(secondExit) + ")");
        }
        auto loaded = motion::load_settings(path);
        require(loaded.has_value(), "concurrent settings writes published torn JSON");
        bool firstRecord = loaded->idleTimeoutSeconds == 701 &&
            loaded->selectedMediaId == "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        bool secondRecord = loaded->idleTimeoutSeconds == 702 &&
            loaded->selectedMediaId == "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        require(firstRecord || secondRecord,
            "concurrent settings writes combined fields from different revisions");
    }

    void corrupt_files_preserve_last_known_good(fs::path const& root)
    {
        auto config = root / L"corrupt-settings.json";
        std::ofstream(config) << "{broken";
        motion::Settings lastKnownGood;
        lastKnownGood.idleTimeoutSeconds = 321;
        require(!motion::try_load_settings(config, lastKnownGood), "corrupt settings were accepted");
        require(lastKnownGood.idleTimeoutSeconds == 321, "corrupt settings destroyed the last-known-good state");

        auto metadata = root / L"corrupt-media.json";
        std::ofstream(metadata) << "not-json";
        motion::MediaMetadata media;
        require(!motion::try_load_media(metadata, media), "corrupt media metadata escaped resilient loading");
    }

    void media_library_operations_are_safe(fs::path const& root)
    {
        char const* phase = "initialize";
        try {
            auto libraryRoot = root / L"library";
            motion::app::MediaLibrary library(libraryRoot, motion::app::DeleteMode::Permanent);
            library.EnsureDirectories();
            phase = "create groups";
            auto first = library.CreateGroup(L"海景", {});
            auto second = library.CreateGroup(L"山谷", { first });

            phase = "reject disguised content";
            auto disguisedVideo = root / L"disguised.mp4";
            std::ofstream(disguisedVideo, std::ios::binary) << "not a media container";
            bool rejected{};
            try { (void)library.Import(disguisedVideo, "video", first.id); }
            catch (...) { rejected = true; }
            require(rejected, "extension-only validation accepted disguised video content");

            phase = "import";
            auto source = root / L"sample.png";
            write_test_bitmap(source);
            auto mediaId = library.Import(source, "image", first.id);
            auto imported = library.LoadMedia(first.id);
            require(imported.size() == 1 && imported.front().id == mediaId, "media import did not round-trip");
            phase = "concurrent cover and rename";
            fs::copy_file(source, library.MediaDirectory(imported.front()) / L"poster.png",
                fs::copy_options::overwrite_existing);
            std::exception_ptr renameError, coverError;
            std::thread rename([&] {
                try { library.Rename(imported.front(), L"清晨海景"); } catch (...) { renameError = std::current_exception(); }
            });
            std::thread cover([&] {
                try { library.UpdateCover(imported.front(), L"poster.png"); } catch (...) { coverError = std::current_exception(); }
            });
            rename.join();
            cover.join();
            if (renameError) std::rethrow_exception(renameError);
            if (coverError) std::rethrow_exception(coverError);
            auto concurrentlyUpdated = library.LoadMedia(first.id);
            if (concurrentlyUpdated.size() != 1 || concurrentlyUpdated.front().name != L"清晨海景" ||
                concurrentlyUpdated.front().coverFileName != L"poster.png" || concurrentlyUpdated.front().revision < 3) {
                auto revision = concurrentlyUpdated.empty() ? 0 : concurrentlyUpdated.front().revision;
                auto coverName = concurrentlyUpdated.empty() ? std::wstring{} : concurrentlyUpdated.front().coverFileName;
                throw std::runtime_error("serialized media operations lost a field update (revision " +
                    std::to_string(revision) + ", cover " + motion::wide_to_utf8(coverName) + ")");
            }
            phase = "move";
            // Intentionally use the stale pre-rename record. Operations resolve
            // by media ID and must not overwrite the latest metadata revision.
            library.Move(imported.front(), second.id);
            require(library.LoadMedia(first.id).empty(), "moved media survived in the source group");
            auto moved = library.LoadMedia(second.id);
            require(moved.size() == 1 && moved.front().groupId == second.id, "moved media was not committed to the target group");
            library.Rename(imported.front(), L"山谷清晨");
            moved = library.LoadMedia(second.id);
            require(moved.size() == 1 && moved.front().name == L"山谷清晨" && moved.front().coverFileName == L"poster.png",
                "a stale asynchronous operation did not follow the moved media safely");
            phase = "variant task lifecycle";
            auto videoRecord = moved.front();
            videoRecord.kind = "video";
            auto mediaDirectory = library.MediaDirectory(videoRecord);
            require(library.RequestOptimization(videoRecord, "power-saver"),
                "video import did not create a durable optimization request");
            auto variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.queued && !variantStatus.cancelled,
                "optimization request was not visible to the UI status model");
            library.PauseOptimization(videoRecord);
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.queued && variantStatus.paused,
                "pausing optimization lost its durable request");
            library.ResumeOptimization(videoRecord);
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.queued && !variantStatus.paused,
                "resuming optimization did not restore its queued state");
            fs::create_directories(mediaDirectory / L"Variants");
            auto partial = mediaDirectory / L"Variants" / L"power-saver-test.part.mp4";
            std::ofstream(partial, std::ios::binary) << "partial";
            library.CancelOptimization(videoRecord);
            variantStatus = library.VariantStatus(videoRecord);
            require(!variantStatus.queued && !variantStatus.paused && variantStatus.cancelled &&
                !fs::exists(partial),
                "cancelling optimization did not clear its request and temporary output");
            require(library.RequestOptimization(videoRecord, "balanced"),
                "a cancelled optimization could not be requested again");
            std::ofstream(mediaDirectory / L"Variants" / L"balanced-test.mp4", std::ios::binary) << "derived-copy";
            std::ofstream(mediaDirectory / L"Variants" / L"power-saver-test.mp4", std::ios::binary) << "low-power";
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.files == 2 && variantStatus.bytes == 21,
                "generated optimization copies were not reported accurately");
            auto sourcePath = mediaDirectory / moved.front().fileName;
            library.SuppressOptimization(videoRecord, "balanced");
            library.DeleteVariantProfile(videoRecord, "balanced");
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.balancedSuppressed && variantStatus.files == 1 &&
                variantStatus.entries.front().mode == "power-saver" && fs::is_regular_file(sourcePath),
                "deleting one performance profile damaged the source or the other profile");
            require(library.RequestOptimization(videoRecord, "balanced") &&
                !library.VariantStatus(videoRecord).balancedSuppressed,
                "manual generation did not re-enable a deleted performance profile");
            std::ofstream(mediaDirectory / L"Variants" / L"balanced-test.mp4", std::ios::binary) << "derived-copy";
            library.SuppressOptimization(videoRecord, "balanced");
            library.SuppressOptimization(videoRecord, "power-saver");
            library.DeleteVariantProfile(videoRecord, "balanced");
            library.DeleteVariantProfile(videoRecord, "power-saver");
            variantStatus = library.VariantStatus(videoRecord);
            require(variantStatus.files == 0 && variantStatus.balancedSuppressed &&
                variantStatus.powerSaverSuppressed && fs::is_regular_file(sourcePath) &&
                !fs::exists(mediaDirectory / L"Variants"),
                "multi-profile deletion damaged the source or left selected copies behind");
            phase = "source-only delete";
            fs::create_directories(mediaDirectory / L"Variants");
            std::ofstream(mediaDirectory / L"Variants" / L"balanced-60-1280x720-v4.mp4",
                std::ios::binary) << "balanced-copy";
            std::ofstream(mediaDirectory / L"Variants" / L"power-saver-60-1280x720-v4.mp4",
                std::ios::binary) << "power-copy";
            auto metadata = motion::load_media(mediaDirectory / L"metadata.json");
            require(metadata.has_value(), "source-delete fixture lost its metadata");
            metadata->kind = "video";
            motion::save_media(mediaDirectory / L"metadata.json", *metadata);
            videoRecord = *metadata;
            library.DeleteSource(videoRecord);
            require(!library.SourceAvailable(videoRecord) &&
                fs::is_regular_file(mediaDirectory / L"metadata.json") &&
                fs::is_regular_file(mediaDirectory / L"poster.png") &&
                library.VariantStatus(videoRecord).entries.size() == 2,
                "deleting the source removed the wallpaper identity, poster, or retained copies");
            require(!library.RequestOptimization(videoRecord, "balanced"),
                "a source-less wallpaper accepted a new optimization request");
            library.DeleteVariantProfile(videoRecord, "balanced");
            bool lastCopyProtected{};
            try { library.DeleteVariantProfile(videoRecord, "power-saver"); }
            catch (...) { lastCopyProtected = true; }
            require(lastCopyProtected && library.VariantStatus(videoRecord).entries.size() == 1,
                "the final playable copy was not protected after source deletion");
            phase = "delete";
            library.Delete(moved.front());
            require(library.LoadMedia(second.id).empty(), "deleted media survived on disk");

            phase = "duplicate group";
            auto duplicate = first;
            duplicate.id = motion::new_id();
            auto duplicateRoot = library.WallpapersPath() / L"Groups" / motion::utf8_to_wide(duplicate.id);
            fs::create_directories(duplicateRoot / L"Videos");
            motion::save_group(duplicateRoot / L"group.json", duplicate);
            std::ofstream(duplicateRoot / L"keep.me") << "must survive";
            auto groups = library.LoadGroups().groups;
            auto sameNameCount = std::count_if(groups.begin(), groups.end(), [](auto const& group) { return _wcsicmp(group.name.c_str(), L"海景") == 0; });
            require(sameNameCount == 2, "duplicate group loading performed an implicit destructive merge");
            require(fs::is_regular_file(duplicateRoot / L"keep.me"), "duplicate group loading deleted an unrecognized file");
        } catch (std::exception const& error) {
            throw std::runtime_error(std::string(phase) + ": " + error.what());
        }
    }

    void xaml_events_are_bound_to_handlers()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream xamlFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml", std::ios::binary);
        std::ifstream cppFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.cpp", std::ios::binary);
        std::ifstream headerFile(sourceRoot / L"MotionWallpaper.App" / L"MainWindow.xaml.h", std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp", std::ios::binary);
        require(static_cast<bool>(xamlFile) && static_cast<bool>(cppFile) && static_cast<bool>(headerFile) && static_cast<bool>(agentFile),
            "UI or Agent source files were not found");
        std::string xaml((std::istreambuf_iterator<char>(xamlFile)), {});
        std::string cpp((std::istreambuf_iterator<char>(cppFile)), {});
        std::string header((std::istreambuf_iterator<char>(headerFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        std::regex event(R"event((?:Click|SelectionChanged|TextChanged|RightTapped|DragItemsStarting|DragItemsCompleted)="([A-Za-z0-9_]+)")event");
        for (std::sregex_iterator found(xaml.begin(), xaml.end(), event), end; found != end; ++found) {
            auto handler = (*found)[1].str();
            require(cpp.find("MainWindow::" + handler + "(") != std::string::npos, "XAML event references a missing handler");
        }
        std::regex declaredEvent(R"event(void ([A-Za-z0-9_]+)\(Windows::Foundation::IInspectable const&)event");
        for (std::sregex_iterator found(header.begin(), header.end(), declaredEvent), end; found != end; ++found) {
            auto handler = (*found)[1].str();
            require(xaml.find("=\"" + handler + "\"") != std::string::npos, "declared UI event handler is no longer bound in XAML");
        }
        require(cpp.find("SelectWallpaperForTarget(media.groupId, media.id, selectedDisplayId)") != std::string::npos,
            "wallpaper card selection no longer updates the persisted selection");
        require(cpp.find("motion::try_load_runtime(path, runtime)") != std::string::npos,
            "wallpaper UI no longer consumes the Agent's acknowledged runtime selection");
        require(xaml.find("x:Name=\"VariantsPage\"") != std::string::npos &&
            xaml.find("Click=\"Variants_Click\"") != std::string::npos,
            "the global performance-copy page is no longer reachable");
        require(xaml.find("GenerateVariantButton") == std::string::npos &&
            xaml.find("DeleteVariantsButton") == std::string::npos,
            "performance-copy actions leaked back into a wallpaper group page");
        require(cpp.find("enum class AppPage") == std::string::npos,
            "page navigation state was duplicated in the implementation file");
        require(agent.find("save_settings(") == std::string::npos,
            "Agent became a second writer of the user settings file");
        require(agent.find("if (targetReady)") != std::string::npos &&
            agent.find("publishRuntime(groupId, mediaId, decode.path, decode.reason)") != std::string::npos,
            "wallpaper runtime state is published before the Renderer ACK");
        require(agent.find("RendererPool") != std::string::npos && agent.find("display_media_targets") != std::string::npos,
            "per-display renderer routing is no longer active");
        require(agent.find("EnumWindows(") != std::string::npos && agent.find("desktop_covered()") != std::string::npos,
            "fullscreen coverage regressed to foreground-window-only detection");
    }

    void windows_app_sdk_dependencies_are_release_safe()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream projectFile(
            sourceRoot / L"MotionWallpaper.App" / L"MotionWallpaper.App.vcxproj", std::ios::binary);
        std::string project((std::istreambuf_iterator<char>(projectFile)), {});
        require(project.find("Microsoft.WindowsAppSDK.Foundation\" Version=\"1.8.260803002") != std::string::npos &&
            project.find("Microsoft.WindowsAppSDK.InteractiveExperiences\" Version=\"1.8.260708001") != std::string::npos &&
            project.find("Microsoft.WindowsAppSDK.WinUI\" Version=\"1.8.260803003") != std::string::npos,
            "release build no longer pins the audited stable Windows App SDK 1.8 components");
        require(project.find("Include=\"Microsoft.WindowsAppSDK\"") == std::string::npos &&
            project.find("Version=\"2.") == std::string::npos,
            "an aggregate or engineering-preview Windows App SDK dependency entered the release project");
    }

    void display_topology_uses_physical_pixels()
    {
        auto sourceRoot = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
        std::ifstream rendererFile(sourceRoot / L"MotionWallpaper.Renderer" / L"Renderer.cpp", std::ios::binary);
        std::ifstream agentFile(sourceRoot / L"MotionWallpaper.Agent" / L"Agent.cpp", std::ios::binary);
        std::ifstream awarenessFile(sourceRoot / L"MotionWallpaper.Common" / L"DisplayAwareness.h", std::ios::binary);
        require(rendererFile && agentFile && awarenessFile, "display topology source files were not found");
        std::string renderer((std::istreambuf_iterator<char>(rendererFile)), {});
        std::string agent((std::istreambuf_iterator<char>(agentFile)), {});
        std::string awareness((std::istreambuf_iterator<char>(awarenessFile)), {});
        require(awareness.find("DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2") != std::string::npos,
            "display coordinates regressed to DPI-virtualized logical pixels");
        require(renderer.find("motion::enable_per_monitor_dpi_awareness();") != std::string::npos &&
            agent.find("motion::enable_per_monitor_dpi_awareness();") != std::string::npos,
            "Renderer and Agent no longer share the same physical-pixel coordinate space");
        require(renderer.find("case WM_DISPLAYCHANGE:") != std::string::npos &&
            renderer.find("case WM_DPICHANGED:") != std::string::npos,
            "Renderer no longer rebuilds after monitor resolution or DPI changes");
    }

    void mixed_resolution_displays_keep_independent_physical_bounds()
    {
        RECT laptop{ 0, 0, 2560, 1600 };
        RECT external{ 2560, 80, 5120, 1520 };
        require(laptop.right - laptop.left == 2560 && laptop.bottom - laptop.top == 1600,
            "laptop output no longer retains its own physical size");
        require(external.right - external.left == 2560 && external.bottom - external.top == 1440,
            "external output was incorrectly stretched to the virtual-desktop height");
        require(external.left == 2560 && external.top == 80,
            "secondary output lost its independent physical origin");
    }

    void active_displays_have_stable_physical_targets()
    {
        auto displays = motion::enumerate_displays();
        require(!displays.empty(), "Windows did not expose an active display target");
        size_t primaryCount{};
        std::vector<std::string> ids;
        for (auto const& display : displays) {
            require(!display.id.empty() && !display.deviceName.empty(), "active display has no stable identity");
            require(display.bounds.right > display.bounds.left && display.bounds.bottom > display.bounds.top,
                "active display has invalid physical bounds");
            require(std::find(ids.begin(), ids.end(), display.id) == ids.end(), "active display identities are not unique");
            ids.push_back(display.id);
            if (display.primary) ++primaryCount;
            auto resolved = motion::find_display_bounds(display.deviceName);
            require(resolved.has_value() && EqualRect(&*resolved, &display.bounds),
                "display device did not resolve back to its physical bounds");
        }
        require(primaryCount == 1, "display topology does not contain exactly one primary monitor");
    }
}

int wmain(int argc, wchar_t** argv)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (argc == 8 && (std::wstring_view(argv[1]) == L"--transcode-video" ||
        std::wstring_view(argv[1]) == L"--transcode-cpu-video")) {
        bool cpuPlayback = std::wstring_view(argv[1]) == L"--transcode-cpu-video";
        std::wstring error;
        auto result = motion::agent::transcode_video(
            argv[2], argv[3], argv[4], static_cast<uint32_t>(_wtoi(argv[5])),
            static_cast<uint32_t>(_wtoi(argv[6])), static_cast<uint32_t>(_wtoi(argv[7])),
            [] { return motion::agent::VideoTranscodeControl::running; }, error,
            nullptr, true, cpuPlayback);
        std::wcout << static_cast<int>(result) << L" " << error << L'\n';
        return result == motion::agent::VideoTranscodeResult::succeeded ? 0 : 3;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--write-settings") {
        motion::Settings settings;
        settings.idleTimeoutSeconds = 777;
        settings.selectedGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        settings.selectedMediaId = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        for (int index = 0; index < 100; ++index) motion::save_settings(argv[2], settings);
        return 0;
    }
    if (argc == 3 && (std::wstring_view(argv[1]) == L"--write-settings-a" ||
        std::wstring_view(argv[1]) == L"--write-settings-b")) {
        bool first = std::wstring_view(argv[1]) == L"--write-settings-a";
        motion::Settings settings;
        settings.idleTimeoutSeconds = first ? 701 : 702;
        settings.selectedGroupId = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        settings.selectedMediaId = first ? "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa" :
            "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
        for (int index = 0; index < 200; ++index) motion::save_settings(argv[2], settings);
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--write-runtime") {
        motion::RuntimeState runtime;
        runtime.activeGroupId = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
        runtime.activeMediaId = "cccccccc-cccc-cccc-cccc-cccccccccccc";
        runtime.decodePath = "software-fallback";
        runtime.decodeReason = "fallback-no-hardware-decoder";
        for (int index = 0; index < 100; ++index) motion::save_runtime(argv[2], runtime);
        return 0;
    }
    auto root = fs::temp_directory_path() / (L"MotionWallpaper.Tests." + motion::utf8_to_wide(motion::new_id()));
    char const* currentTest = "startup";
#define RUN_TEST(expression) do { currentTest = #expression; expression; } while (false)
    try {
        fs::create_directories(root);
        RUN_TEST(application_data_location_preserves_portable_and_legacy_libraries(root));
        RUN_TEST(settings_round_trip_clears_empty_values(root));
        RUN_TEST(legacy_settings_are_migrated(root));
        RUN_TEST(coupled_lock_and_display_off_settings_are_migrated(root));
        RUN_TEST(unsafe_media_paths_are_rejected(root));
        RUN_TEST(desktop_state_is_deterministic());
        RUN_TEST(presentation_state_has_explicit_priorities());
        RUN_TEST(identifiers_are_path_safe());
        RUN_TEST(future_settings_are_rejected(root));
        RUN_TEST(media_activity_suspends_idle_time());
        RUN_TEST(audio_allows_screensaver_but_defers_automatic_lock());
        RUN_TEST(external_media_remains_authoritative_during_own_screensaver());
        RUN_TEST(display_off_waits_for_the_post_lock_delay());
        RUN_TEST(fullscreen_coverage_is_not_limited_to_foreground());
        RUN_TEST(normal_pause_keeps_decoder_hot());
        RUN_TEST(stable_agent_states_do_not_poll_at_twenty_hertz());
        RUN_TEST(battery_power_pauses_optional_variant_generation());
        RUN_TEST(playback_capability_only_degrades_software_devices());
        RUN_TEST(software_presentation_governor_recovers_without_catchup_bursts());
        RUN_TEST(screensaver_pause_returns_window_to_desktop());
        RUN_TEST(desktop_host_must_cover_the_virtual_screen());
        RUN_TEST(manual_selection_wins_over_group_randomization());
        RUN_TEST(identical_media_share_one_renderer());
        RUN_TEST(video_variant_policy_preserves_quality_priority());
        RUN_TEST(variant_requests_use_last_writer_wins(root));
        RUN_TEST(video_transcoder_fails_closed_without_backend(root));
        RUN_TEST(video_transcoder_orders_vendor_backends_and_bounds_software_fallback());
        RUN_TEST(frame_scheduler_uses_real_interval());
        RUN_TEST(adapter_policy_preserves_heavy_video_throughput());
        RUN_TEST(renderer_ack_channels_are_isolated());
        RUN_TEST(decode_modes_have_distinct_fallback_contracts());
        RUN_TEST(renderer_process_uses_typed_acks(root));
        RUN_TEST(renderer_exits_when_agent_pipe_closes(root));
        RUN_TEST(settings_and_runtime_have_single_writers(root));
        RUN_TEST(automatic_decode_runtime_round_trips(root));
        RUN_TEST(concurrent_settings_writers_never_publish_torn_json(root));
        RUN_TEST(corrupt_files_preserve_last_known_good(root));
        RUN_TEST(media_library_operations_are_safe(root));
        RUN_TEST(xaml_events_are_bound_to_handlers());
        RUN_TEST(windows_app_sdk_dependencies_are_release_safe());
        RUN_TEST(display_topology_uses_physical_pixels());
        RUN_TEST(mixed_resolution_displays_keep_independent_physical_bounds());
        RUN_TEST(active_displays_have_stable_physical_targets());
        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::wcout << L"MotionWallpaper native tests passed\n";
        return 0;
    } catch (std::exception const& error) {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::cerr << "MotionWallpaper native tests failed in " << currentTest << ": " << error.what() << '\n';
        return 1;
    }
#undef RUN_TEST
}
