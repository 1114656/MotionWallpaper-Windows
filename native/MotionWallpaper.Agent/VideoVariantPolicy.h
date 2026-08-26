#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace motion::agent
{
    [[nodiscard]] constexpr bool video_cpu_conversion_allowed(
        bool hdrTransfer, bool bt2020Primaries) noexcept
    {
        return !hdrTransfer && !bt2020Primaries;
    }

    enum class VideoSourceCodec { Unknown, H264, Hevc };

    [[nodiscard]] constexpr bool video_software_fallback_allowed(
        VideoSourceCodec codec, bool profileKnown, uint32_t profile,
        bool hdrTransfer = false, bool bt2020Primaries = false) noexcept
    {
        if (hdrTransfer || bt2020Primaries) return false;
        // Kvazaar produces 8-bit HEVC Main. Unknown HEVC profiles and known
        // Main10/high-bit-depth profiles must stay source-backed when hardware
        // encoding is unavailable; silently changing their bit depth is a
        // quality and HDR correctness failure.
        if (codec == VideoSourceCodec::Hevc) return profileKnown && profile == 1;
        if (codec == VideoSourceCodec::H264 && profileKnown && profile >= 110) return false;
        return true;
    }

    struct VideoVariantDecision
    {
        uint32_t targetFps{};
        std::wstring fileName;
    };

    [[nodiscard]] inline VideoVariantDecision video_variant_decision(
        std::string const& performanceMode,
        uint32_t width = 0, uint32_t height = 0,
        uint32_t sourceRateNumerator = 0, uint32_t sourceRateDenominator = 0,
        uint32_t displayRefreshRate = 0)
    {
        if (performanceMode == "original") return {};
        bool cpuSmooth = performanceMode == "cpu-smooth";
        uint32_t cap = performanceMode == "power-saver" || cpuSmooth
            ? (std::min)(60u, displayRefreshRate ? displayRefreshRate : 60u)
            : (std::min)(120u, displayRefreshRate ? displayRefreshRate : 120u);
        uint32_t sourceFps = sourceRateNumerator && sourceRateDenominator
            ? static_cast<uint32_t>((static_cast<uint64_t>(sourceRateNumerator) + sourceRateDenominator / 2) /
                sourceRateDenominator)
            : cap;
        uint32_t targetFps = (std::max)(1u, (std::min)(sourceFps, cap));
        auto dimensions = width && height
            ? L"-" + std::to_wstring(width) + L"x" + std::to_wstring(height)
            : std::wstring{};
        if (cpuSmooth) {
            return { targetFps, L"cpu-smooth-" + std::to_wstring(targetFps) + dimensions + L"-v5.mp4" };
        }
        if (performanceMode == "power-saver") {
            return { targetFps, L"power-saver-" + std::to_wstring(targetFps) + dimensions + L"-v4.mp4" };
        }
        return { targetFps, L"balanced-" + std::to_wstring(targetFps) + dimensions + L"-v4.mp4" };
    }

    [[nodiscard]] constexpr std::pair<uint32_t, uint32_t> video_variant_dimensions(
        uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t targetWidth, uint32_t targetHeight) noexcept
    {
        if (!sourceWidth || !sourceHeight || !targetWidth || !targetHeight ||
            targetWidth >= sourceWidth || targetHeight >= sourceHeight) {
            return { sourceWidth, sourceHeight };
        }
        auto ceil_div = [](uint64_t value, uint64_t divisor) {
            return static_cast<uint32_t>((value + divisor - 1) / divisor);
        };
        uint32_t width{}, height{};
        if (static_cast<uint64_t>(targetWidth) * sourceHeight >=
            static_cast<uint64_t>(targetHeight) * sourceWidth) {
            width = targetWidth;
            height = ceil_div(static_cast<uint64_t>(sourceHeight) * targetWidth, sourceWidth);
        } else {
            height = targetHeight;
            width = ceil_div(static_cast<uint64_t>(sourceWidth) * targetHeight, sourceHeight);
        }
        width = (width + 1u) & ~1u;
        height = (height + 1u) & ~1u;
        return { (std::min)(width, sourceWidth), (std::min)(height, sourceHeight) };
    }

    // CPU playback has a hard pixel budget. Fit the complete source inside it
    // and let the existing Renderer crop/scale for the display. Unlike the
    // quality profiles, this may upscale at presentation time because decoding
    // an oversized off-screen area defeats the compatibility fallback.
    [[nodiscard]] constexpr std::pair<uint32_t, uint32_t> video_cpu_variant_dimensions(
        uint32_t sourceWidth, uint32_t sourceHeight,
        uint32_t maximumWidth, uint32_t maximumHeight) noexcept
    {
        if (!sourceWidth || !sourceHeight || !maximumWidth || !maximumHeight ||
            (sourceWidth <= maximumWidth && sourceHeight <= maximumHeight)) {
            return { sourceWidth, sourceHeight };
        }
        uint32_t width{};
        uint32_t height{};
        if (static_cast<uint64_t>(maximumWidth) * sourceHeight <=
            static_cast<uint64_t>(maximumHeight) * sourceWidth) {
            width = maximumWidth;
            height = static_cast<uint32_t>((static_cast<uint64_t>(sourceHeight) * maximumWidth +
                sourceWidth - 1) / sourceWidth);
        } else {
            height = maximumHeight;
            width = static_cast<uint32_t>((static_cast<uint64_t>(sourceWidth) * maximumHeight +
                sourceHeight - 1) / sourceHeight);
        }
        width = (std::max)(2u, (width + 1u) & ~1u);
        height = (std::max)(2u, (height + 1u) & ~1u);
        return { (std::min)(width, sourceWidth), (std::min)(height, sourceHeight) };
    }

    [[nodiscard]] inline bool video_needs_variant(
        uint32_t numerator, uint32_t denominator, uint32_t targetFps,
        uint32_t sourceWidth = 0, uint32_t sourceHeight = 0,
        uint32_t targetWidth = 0, uint32_t targetHeight = 0) noexcept
    {
        if (!numerator || !denominator || !targetFps) return false;
        bool frameRateDiffers = static_cast<uint64_t>(numerator) >
            static_cast<uint64_t>(targetFps) * denominator;
        bool dimensionsDiffer = sourceWidth && sourceHeight && targetWidth && targetHeight &&
            (sourceWidth != targetWidth || sourceHeight != targetHeight);
        return frameRateDiffers || dimensionsDiffer;
    }

    [[nodiscard]] inline bool video_variant_rate_matches(
        uint32_t numerator, uint32_t denominator, uint32_t targetFps) noexcept
    {
        if (!numerator || !denominator || !targetFps) return false;
        auto expected = static_cast<uint64_t>(targetFps) * denominator;
        auto actual = static_cast<uint64_t>(numerator);
        auto delta = actual > expected ? actual - expected : expected - actual;
        return delta <= denominator;
    }

    [[nodiscard]] inline bool video_variant_dimensions_match(
        uint32_t actualWidth, uint32_t actualHeight,
        uint32_t visibleWidth, uint32_t visibleHeight) noexcept
    {
        if (!actualWidth || !actualHeight || !visibleWidth || !visibleHeight) return false;
        auto coded = [](uint32_t value) { return (value + 31u) & ~31u; };
        return (actualWidth == visibleWidth || actualWidth == coded(visibleWidth)) &&
            (actualHeight == visibleHeight || actualHeight == coded(visibleHeight));
    }
}
