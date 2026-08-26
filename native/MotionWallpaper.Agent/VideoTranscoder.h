#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace motion::agent
{
    enum class VideoTranscodeControl { running, paused, cancelled };
    enum class VideoTranscodeResult { succeeded, unsupported, paused, cancelled, failed };

    enum class VideoTranscodeBackend
    {
        nvidiaCudaNvenc,
        nvidiaNvenc,
        intelQsv,
        amdAmf,
        softwareKvazaar,
        softwareOpenH264
    };

    struct VideoTranscodeAdapter
    {
        uint32_t vendorId{};
        uint64_t dedicatedVideoMemory{};
    };

    [[nodiscard]] std::vector<VideoTranscodeBackend> video_transcode_backend_order(
        std::vector<VideoTranscodeAdapter> adapters,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        bool adapterProbeSucceeded = true,
        bool softwareFallbackAllowed = true,
        bool softwarePlaybackTarget = false);

    [[nodiscard]] std::wstring video_transcode_backend_name(VideoTranscodeBackend backend);

    VideoTranscodeResult transcode_video(
        std::filesystem::path const& ffmpeg,
        std::filesystem::path const& source,
        std::filesystem::path const& destination,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        std::function<VideoTranscodeControl()> const& control,
        std::wstring& error,
        std::wstring* selectedBackend = nullptr,
        bool softwareFallbackAllowed = true,
        bool softwarePlaybackTarget = false);
}
