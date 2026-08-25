#pragma once

#include "MediaLibrary.h"

#include <string>
#include <vector>

namespace motion::app
{
    struct VariantProfileSummary
    {
        uint32_t files{};
        uint64_t bytes{};
        bool sharedStorage{};
    };

    struct VariantMediaSummary
    {
        motion::MediaMetadata media;
        std::wstring groupName;
        motion::VariantCacheStatus status;
        VariantProfileSummary balanced;
        VariantProfileSummary powerSaver;
        bool sourceAvailable{};
    };

    [[nodiscard]] inline std::vector<VariantMediaSummary> load_variant_page(
        MediaLibrary& library, std::vector<motion::GroupMetadata> const& groups)
    {
        std::vector<VariantMediaSummary> result;
        for (auto const& group : groups) {
            auto media = library.LoadMedia(group.id);
            for (auto& item : media) {
                if (item.kind != "video") continue;
                VariantMediaSummary summary;
                summary.media = std::move(item);
                summary.groupName = group.name;
                summary.status = library.VariantStatus(summary.media);
                summary.sourceAvailable = library.SourceAvailable(summary.media);
                for (auto const& entry : summary.status.entries) {
                    auto* profile = entry.mode == "balanced" ? &summary.balanced
                        : entry.mode == "power-saver" ? &summary.powerSaver : nullptr;
                    if (!profile) continue;
                    ++profile->files;
                    profile->bytes += entry.bytes;
                    profile->sharedStorage = profile->sharedStorage || entry.sharedStorage;
                }
                result.push_back(std::move(summary));
            }
        }
        return result;
    }
}
