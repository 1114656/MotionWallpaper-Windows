#include "pch.h"
#include "SettingsStore.h"

namespace motion::app
{
    bool SettingsStore::ApplyStartup(bool enabled) const noexcept
    {
        constexpr wchar_t key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (!enabled) {
            auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"MotionWallpaper");
            return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
        }
        std::wstring command = L"\"" + agentPath_.wstring() + L"\"";
        auto result = RegSetKeyValueW(HKEY_CURRENT_USER, key, L"MotionWallpaper", REG_SZ,
            command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        return result == ERROR_SUCCESS;
    }

    motion::Settings SettingsStore::Load() const
    {
        auto settings = motion::load_settings(path_).value_or(motion::Settings{});
        motion::save_settings(path_, settings); // Canonicalize legacy keys and remove dead settings.
        ApplyStartup(settings.startWithWindows);
        return settings;
    }

    bool SettingsStore::Save(motion::Settings const& settings) const
    {
        motion::save_settings(path_, settings);
        ApplyStartup(settings.startWithWindows);
        return motion::notify_settings_changed();
    }
}
