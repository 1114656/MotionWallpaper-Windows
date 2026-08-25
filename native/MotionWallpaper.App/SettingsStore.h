#pragma once

#include "../MotionWallpaper.Common/Common.h"

namespace motion::app
{
    class SettingsStore
    {
    public:
        explicit SettingsStore(std::filesystem::path root)
            : path_(root / L"Config" / L"settings.json"), agentPath_(std::move(root) / L"motionwallpaper-agent.exe") {}
        motion::Settings Load() const;
        bool Save(motion::Settings const& settings) const;
    private:
        bool ApplyStartup(bool enabled) const noexcept;
        std::filesystem::path path_;
        std::filesystem::path agentPath_;
    };
}
