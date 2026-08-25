#include "DisplayTopology.h"
#include "TextEncoding.h"

#include <algorithm>

namespace motion
{
    namespace
    {
        BOOL CALLBACK collect_display(HMONITOR monitor, HDC, RECT*, LPARAM parameter)
        {
            auto& displays = *reinterpret_cast<std::vector<DisplayTarget>*>(parameter);
            MONITORINFOEXW info{ sizeof(info) };
            if (!GetMonitorInfoW(monitor, &info)) return TRUE;

            DISPLAY_DEVICEW monitorDevice{ sizeof(monitorDevice) };
            bool foundDevice{};
            for (DWORD index = 0; EnumDisplayDevicesW(info.szDevice, index, &monitorDevice,
                EDD_GET_DEVICE_INTERFACE_NAME); ++index) {
                if (monitorDevice.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                    foundDevice = true;
                    break;
                }
                monitorDevice = { sizeof(monitorDevice) };
            }

            DisplayTarget target;
            target.deviceName = info.szDevice;
            target.bounds = info.rcMonitor;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) &&
                mode.dmDisplayFrequency > 1) {
                target.refreshRateHz = mode.dmDisplayFrequency;
            }
            target.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            target.friendlyName = foundDevice && monitorDevice.DeviceString[0]
                ? monitorDevice.DeviceString : info.szDevice;
            target.id = utf8_from_wide(foundDevice && monitorDevice.DeviceID[0]
                ? std::wstring_view(monitorDevice.DeviceID) : std::wstring_view(info.szDevice));
            displays.push_back(std::move(target));
            return TRUE;
        }
    }

    std::vector<DisplayTarget> enumerate_displays()
    {
        std::vector<DisplayTarget> displays;
        EnumDisplayMonitors(nullptr, nullptr, collect_display, reinterpret_cast<LPARAM>(&displays));
        std::stable_sort(displays.begin(), displays.end(), [](auto const& left, auto const& right) {
            if (left.primary != right.primary) return left.primary;
            if (left.bounds.left != right.bounds.left) return left.bounds.left < right.bounds.left;
            return left.bounds.top < right.bounds.top;
        });
        for (size_t index = 0; index < displays.size(); ++index) {
            if (displays[index].friendlyName.empty()) displays[index].friendlyName = L"显示器 " + std::to_wstring(index + 1);
        }
        return displays;
    }

    std::optional<RECT> find_display_bounds(std::wstring const& deviceName) noexcept
    {
        try {
            auto displays = enumerate_displays();
            auto found = std::find_if(displays.begin(), displays.end(), [&](auto const& display) {
                return _wcsicmp(display.deviceName.c_str(), deviceName.c_str()) == 0;
            });
            if (found != displays.end()) return found->bounds;
        } catch (...) {}
        return std::nullopt;
    }

    RECT primary_display_bounds() noexcept
    {
        try {
            auto displays = enumerate_displays();
            auto primary = std::find_if(displays.begin(), displays.end(), [](auto const& display) { return display.primary; });
            if (primary != displays.end()) return primary->bounds;
        } catch (...) {}
        return { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
}
