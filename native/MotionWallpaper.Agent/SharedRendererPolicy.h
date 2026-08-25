#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace motion::agent
{
    struct RendererRoute
    {
        std::wstring mediaKey;
        std::wstring monitorDevice;
        std::wstring adapterKey;
    };

    struct SharedRendererRoute
    {
        std::wstring mediaKey;
        std::wstring adapterKey;
        std::vector<std::wstring> monitorDevices;
    };

    [[nodiscard]] inline std::wstring renderer_media_key(
        std::filesystem::path const& path, std::string const& kind)
    {
        return path.wstring() + L"\n" + std::wstring(kind.begin(), kind.end());
    }

    [[nodiscard]] inline std::vector<SharedRendererRoute> group_renderer_routes(
        std::vector<RendererRoute> const& routes, bool includeMonitorDevices)
    {
        using GroupKey = std::pair<std::wstring, std::wstring>;
        std::map<GroupKey, std::vector<std::wstring>> grouped;
        for (auto const& route : routes) {
            auto& monitors = grouped[{ route.mediaKey, route.adapterKey }];
            if (includeMonitorDevices) monitors.push_back(route.monitorDevice);
        }

        std::vector<SharedRendererRoute> result;
        result.reserve(grouped.size());
        for (auto& entry : grouped) {
            result.push_back({ entry.first.first, entry.first.second, std::move(entry.second) });
        }
        return result;
    }
}
