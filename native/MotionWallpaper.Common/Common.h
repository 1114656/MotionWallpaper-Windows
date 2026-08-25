#pragma once

#include <windows.h>
#include "UniqueHandle.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace motion
{
    inline constexpr wchar_t settings_event_name[] = L"Local\\MotionWallpaper.SettingsChanged";
    inline constexpr wchar_t app_exit_event_name[] = L"Local\\MotionWallpaper.ExitRequested";
    inline constexpr int settings_schema_version = 8;
    inline constexpr int runtime_schema_version = 1;
    inline constexpr int group_schema_version = 1;
    inline constexpr int media_schema_version = 2;

    struct DisplayAssignment
    {
        std::string displayId;
        std::string groupId;
        std::string mediaId;
    };

    struct Settings
    {
        int version{ settings_schema_version };
        bool desktopPlayback{ true };
        bool activePlaybackEnabled{ true };
        bool continueWhenCovered{ false };
        bool screensaverEnabled{ true };
        int idleTimeoutSeconds{ 30 };
        bool autoLockEnabled{ true };
        int autoLockTimeoutSeconds{ 300 };
        bool displayOffAfterLockEnabled{ true };
        int displayOffAfterLockDelaySeconds{ 30 };
        std::string decodeMode{ "auto" };
        std::string performanceMode{ "balanced" };
        std::string selectedGroupId;
        std::string selectedMediaId;
        std::string randomGroupId;
        int randomIntervalMinutes{};
        bool startWithWindows{};
        std::string displayMode{ "independent" };
        std::vector<DisplayAssignment> displayAssignments;
    };

    struct GroupMetadata
    {
        int version{ group_schema_version };
        std::string id;
        std::wstring name;
        int order{};
        std::wstring createdAt;
        std::wstring updatedAt;
    };

    struct RuntimeState
    {
        int version{ runtime_schema_version };
        std::string activeGroupId;
        std::string activeMediaId;
        std::string decodePath;
        std::string decodeReason;
        std::wstring updatedAt;
    };

    struct MediaMetadata
    {
        int version{ media_schema_version };
        std::string id;
        std::string groupId;
        std::wstring name;
        std::wstring originalName;
        std::wstring fileName;
        std::string kind{ "video" };
        std::wstring coverFileName;
        std::wstring sha256;
        uint64_t sizeBytes{};
        uint64_t revision{};
        std::wstring importedAt;
        std::wstring updatedAt;
    };

    enum class DesktopIntent { Off, Play, Freeze, Pause };

    class IdleTimer
    {
    public:
        std::chrono::milliseconds Update(
            std::chrono::milliseconds now,
            uint32_t inputTick,
            std::chrono::milliseconds rawIdle,
            bool activityInhibitsIdle) noexcept;

    private:
        bool initialized_{};
        uint32_t inputTick_{};
        std::chrono::milliseconds idleSince_{};
    };

    std::filesystem::path executable_directory();
    std::wstring utf8_to_wide(std::string const& value);
    std::string wide_to_utf8(std::wstring_view value);
    std::string new_id();
    std::wstring timestamp_utc();
    bool valid_id(std::string const& value);
    bool valid_id(std::wstring const& value);
    bool safe_file_name(std::filesystem::path const& value);
    void append_utf8_log(std::filesystem::path const& path, std::wstring_view message) noexcept;
    std::wstring quote_command_line_argument(std::wstring_view value);
    std::wstring build_command_line(std::vector<std::wstring> const& arguments);

    std::optional<Settings> load_settings(std::filesystem::path const& path);
    bool try_load_settings(std::filesystem::path const& path, Settings& destination) noexcept;
    void save_settings(std::filesystem::path const& path, Settings const& settings);
    std::optional<RuntimeState> load_runtime(std::filesystem::path const& path);
    bool try_load_runtime(std::filesystem::path const& path, RuntimeState& destination) noexcept;
    void save_runtime(std::filesystem::path const& path, RuntimeState const& runtime);
    std::optional<GroupMetadata> load_group(std::filesystem::path const& path);
    void save_group(std::filesystem::path const& path, GroupMetadata const& group);
    std::optional<MediaMetadata> load_media(std::filesystem::path const& path);
    bool try_load_media(std::filesystem::path const& path, MediaMetadata& destination) noexcept;
    void save_media(std::filesystem::path const& path, MediaMetadata const& media);

    DesktopIntent desktop_intent(Settings const& settings, bool covered, bool hasMedia);
    bool notify_settings_changed();
}
