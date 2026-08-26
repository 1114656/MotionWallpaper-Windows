#include "VideoOptimizer.h"

#include "VideoTranscoder.h"
#include "VideoVariantPolicy.h"
#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/VariantCache.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace winrt;

namespace
{
    struct SourceRate
    {
        uint32_t numerator{};
        uint32_t denominator{};
        uint32_t width{};
        uint32_t height{};
        bool softwareFallbackAllowed{ true };
        bool softwarePlaybackConversionAllowed{ true };
        bool softwarePlaybackFriendly{};
    };

    SourceRate source_rate(fs::path const& source)
    {
        com_ptr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), nullptr, reader.put()))) return {};
        com_ptr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, type.put()))) return {};
        SourceRate result;
        if (FAILED(MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &result.numerator, &result.denominator))) return {};
        MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &result.width, &result.height);
        GUID subtype{};
        UINT32 profile{};
        bool subtypeKnown = SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype));
        bool hasProfile = SUCCEEDED(type->GetUINT32(MF_MT_MPEG2_PROFILE, &profile));
        UINT32 transferFunction{};
        bool hdrTransfer = SUCCEEDED(type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transferFunction)) &&
            (transferFunction == MFVideoTransFunc_2084 || transferFunction == MFVideoTransFunc_HLG);
        UINT32 primaries{};
        bool bt2020Primaries = SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_PRIMARIES, &primaries)) &&
            primaries == MFVideoPrimaries_BT2020;
        auto codec = subtypeKnown && subtype == MFVideoFormat_HEVC
            ? motion::agent::VideoSourceCodec::Hevc
            : subtypeKnown && subtype == MFVideoFormat_H264
                ? motion::agent::VideoSourceCodec::H264
                : motion::agent::VideoSourceCodec::Unknown;
        result.softwareFallbackAllowed = motion::agent::video_software_fallback_allowed(
            codec, hasProfile, profile, hdrTransfer, bt2020Primaries);
        // The internal CPU playback copy may reduce SDR 10-bit material to
        // 8-bit because the source remains untouched. HDR/BT.2020 requires an
        // explicit tone-mapping policy and must never be silently flattened.
        result.softwarePlaybackConversionAllowed = motion::agent::video_cpu_conversion_allowed(
            hdrTransfer, bt2020Primaries);
        result.softwarePlaybackFriendly = codec == motion::agent::VideoSourceCodec::H264 &&
            result.softwareFallbackAllowed;
        return result;
    }

    bool current_variant(fs::path const& source, fs::path const& variant)
    {
        std::error_code error;
        if (!fs::is_regular_file(variant, error) || error || !fs::file_size(variant, error) || error) return false;
        auto sourceTime = fs::last_write_time(source, error);
        if (error) return false;
        auto variantTime = fs::last_write_time(variant, error);
        return !error && variantTime >= sourceTime;
    }

    bool reuse_equivalent_variant(fs::path const& source, fs::path const& destination,
        std::string const& mode)
    {
        if (mode == "cpu-smooth") return false;
        auto name = destination.filename().wstring();
        auto prefix = mode == "power-saver" ? std::wstring(L"power-saver-") : std::wstring(L"balanced-");
        auto alternatePrefix = mode == "power-saver" ? std::wstring(L"balanced-") : std::wstring(L"power-saver-");
        if (!name.starts_with(prefix)) return false;
        auto alternate = destination.parent_path() / (alternatePrefix + name.substr(prefix.size()));
        if (!current_variant(source, alternate)) return false;
        std::error_code ignored;
        fs::create_directories(destination.parent_path(), ignored);
        fs::remove(destination, ignored);
        return CreateHardLinkW(destination.c_str(), alternate.c_str(), nullptr) &&
            current_variant(source, destination);
    }

    bool on_battery()
    {
        SYSTEM_POWER_STATUS status{};
        return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
    }

    void append_log(fs::path const& root, std::wstring const& message)
    {
        motion::append_utf8_log(root / L"Config" / L"agent.log", message);
    }

    constexpr uint64_t variantCacheLimit = 8ULL * 1024 * 1024 * 1024;
    constexpr uint64_t diskReserve = 256ULL * 1024 * 1024;

    bool has_transcode_space(fs::path const& directory, fs::path const& source)
    {
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr)) return false;
        std::error_code error;
        auto sourceSize = fs::file_size(source, error);
        if (error) return false;
        auto worstCase = sourceSize > (UINT64_MAX - diskReserve) / 6
            ? UINT64_MAX : sourceSize * 6 + diskReserve;
        return available.QuadPart >= worstCase;
    }

    void prune_variant_cache(fs::path const& root, std::set<fs::path> const& protectedFiles)
    {
        struct Candidate
        {
            fs::path path;
            uint64_t size{};
            fs::file_time_type modified{};
        };
        std::vector<Candidate> candidates;
        uint64_t total{};
        std::error_code error;
        auto wallpapers = root / L"Wallpapers";
        fs::recursive_directory_iterator iterator(wallpapers,
            fs::directory_options::skip_permission_denied, error), end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code itemError;
            if (!iterator->is_regular_file(itemError) || itemError ||
                iterator->path().parent_path().filename() != L"Variants") continue;
            auto size = iterator->file_size(itemError);
            if (itemError) continue;
            total += size;
            auto mediaDirectory = iterator->path().parent_path().parent_path();
            motion::MediaMetadata metadata;
            if (motion::try_load_media(mediaDirectory / L"metadata.json", metadata)) {
                std::error_code sourceError;
                if (!fs::is_regular_file(mediaDirectory / metadata.fileName, sourceError) || sourceError) {
                    // Once the source is deliberately removed, retained variants
                    // become the wallpaper's only playable media and are not an
                    // evictable cache anymore.
                    continue;
                }
            }
            if (!protectedFiles.contains(iterator->path())) {
                candidates.push_back({ iterator->path(), size,
                    iterator->last_write_time(itemError) });
            }
        }
        if (total <= variantCacheLimit) return;
        std::sort(candidates.begin(), candidates.end(), [](auto const& left, auto const& right) {
            return left.modified < right.modified;
        });
        for (auto const& candidate : candidates) {
            if (total <= variantCacheLimit) break;
            std::error_code removeError;
            if (fs::remove(candidate.path, removeError) && !removeError) total -= candidate.size;
        }
    }

    std::wstring variant_prefix(std::string const& mode)
    {
        return mode == "balanced" ? L"balanced-" :
            mode == "power-saver" ? L"power-saver-" : L"cpu-smooth-";
    }

    void coalesce_equivalent_profiles(fs::path const& variants) noexcept
    {
        try {
            fs::path balanced, powerSaver;
            std::error_code error;
            for (fs::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (name.starts_with(L"balanced-") && name.ends_with(L"-v4.mp4")) balanced = entries->path();
                else if (name.starts_with(L"power-saver-") && name.ends_with(L"-v4.mp4")) powerSaver = entries->path();
            }
            if (balanced.empty() || powerSaver.empty()) return;
            auto balancedName = balanced.filename().wstring();
            auto powerSaverName = powerSaver.filename().wstring();
            if (balancedName.substr(std::wstring(L"balanced-").size()) !=
                powerSaverName.substr(std::wstring(L"power-saver-").size())) return;

            auto temporary = powerSaver;
            temporary += L".link.tmp";
            fs::remove(temporary, error);
            if (!CreateHardLinkW(temporary.c_str(), balanced.c_str(), nullptr)) return;
            if (!MoveFileExW(temporary.c_str(), powerSaver.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                fs::remove(temporary, error);
            }
        } catch (...) {}
    }

    void normalize_variant_profiles(fs::path const& root)
    {
        std::set<fs::path> directories;
        std::error_code error;
        fs::recursive_directory_iterator iterator(root / L"Wallpapers",
            fs::directory_options::skip_permission_denied, error), end;
        for (; !error && iterator != end; iterator.increment(error)) {
            std::error_code itemError;
            if (iterator->is_regular_file(itemError) && !itemError &&
                iterator->path().parent_path().filename() == L"Variants") {
                directories.insert(iterator->path().parent_path());
            }
        }

        struct Candidate
        {
            fs::path path;
            bool currentPolicy{};
            fs::file_time_type modified{};
        };
        for (auto const& variants : directories) {
            // No transcoder exists while the optimizer is being constructed.
            // Any partial at this point belongs to a crashed or cancelled run.
            motion::remove_variant_partials(variants.parent_path());
            for (auto const& mode : { std::string("balanced"), std::string("power-saver"),
                std::string("cpu-smooth") }) {
                std::optional<Candidate> keep;
                std::error_code entriesError;
                for (fs::directory_iterator entries(variants, entriesError), entriesEnd;
                    !entriesError && entries != entriesEnd; entries.increment(entriesError)) {
                    std::error_code itemError;
                    if (!entries->is_regular_file(itemError) || itemError) continue;
                    auto name = entries->path().filename().wstring();
                    if (!name.starts_with(variant_prefix(mode)) || name.ends_with(L".part.mp4") ||
                        entries->path().extension() != L".mp4") continue;
                    Candidate candidate{ entries->path(), mode == "cpu-smooth"
                        ? name.ends_with(L"-v5.mp4") : name.ends_with(L"-v4.mp4"),
                        entries->last_write_time(itemError) };
                    if (itemError) continue;
                    if (!keep || (candidate.currentPolicy && !keep->currentPolicy) ||
                        (candidate.currentPolicy == keep->currentPolicy && candidate.modified > keep->modified)) {
                        keep = std::move(candidate);
                    }
                }
                if (keep) motion::retain_variant_profile(
                    variants.parent_path(), mode, keep->path.filename().wstring());
            }
            coalesce_equivalent_profiles(variants);
        }
    }
}

namespace motion::agent
{
    struct VideoOptimizer::Impl
    {
        struct Request
        {
            fs::path source;
            fs::path destination;
            uint32_t targetFps{};
            uint32_t width{};
            uint32_t height{};
            std::string mode;
            uint64_t generation{};
            bool explicitRequest{};
            bool softwareFallbackAllowed{ true };
            bool softwarePlaybackTarget{};

            [[nodiscard]] std::wstring Key() const
            {
                return source.wstring() + L"\n" + destination.filename().wstring();
            }
        };

        Impl(fs::path dataRoot, fs::path applicationRoot)
            : dataRoot_(std::move(dataRoot)),
              ffmpeg_(motion::ffmpeg_executable_path(applicationRoot))
        {
            normalize_variant_profiles(dataRoot_);
            mediaFoundationStarted_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
            if (mediaFoundationStarted_) {
                worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
            } else {
                append_log(dataRoot_, L"无法初始化媒体优化器，继续使用原视频。");
            }
        }

        ~Impl()
        {
            worker_.request_stop();
            generation_.fetch_add(1, std::memory_order_relaxed);
            condition_.notify_all();
            if (worker_.joinable()) worker_.join();
            if (mediaFoundationStarted_) MFShutdown();
        }

        fs::path Resolve(fs::path const& source, std::string const& requestedMode,
            uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate,
            bool softwarePlaybackTarget)
        {
            auto mode = softwarePlaybackTarget ? std::string("cpu-smooth") : requestedMode;
            if (source.empty() || mode == "original" || !mediaFoundationStarted_) return source;
            if (fs::is_regular_file(motion::variant_cancelled_path(source.parent_path()))) return source;
            if (motion::variant_generation_paused(source.parent_path())) return source;
            if (motion::variant_generation_suppressed(source.parent_path(), mode)) return source;
            SourceRate rate;
            {
                std::lock_guard lock(mutex_);
                auto found = rates_.find(source.wstring());
                if (found != rates_.end()) rate = found->second;
            }
            if (!rate.numerator) {
                rate = source_rate(source);
                std::lock_guard lock(mutex_);
                rates_[source.wstring()] = rate;
            }

            auto dimensions = softwarePlaybackTarget
                ? video_cpu_variant_dimensions(rate.width, rate.height, targetWidth, targetHeight)
                : video_variant_dimensions(rate.width, rate.height, targetWidth, targetHeight);
            auto decision = video_variant_decision(
                mode, dimensions.first, dimensions.second,
                rate.numerator, rate.denominator, targetRefreshRate);
            bool codecNeedsVariant = softwarePlaybackTarget && !rate.softwarePlaybackFriendly;
            if (!video_needs_variant(rate.numerator, rate.denominator, decision.targetFps,
                rate.width, rate.height, dimensions.first, dimensions.second) && !codecNeedsVariant) return source;
            if (softwarePlaybackTarget && !rate.softwarePlaybackConversionAllowed) return source;
            auto destination = source.parent_path() / L"Variants" / decision.fileName;
            auto key = source.wstring() + L"\n" + decision.fileName;

            fs::path chosenPath;
            {
                std::lock_guard lock(mutex_);
                auto chosen = choices_.find(key);
                if (chosen != choices_.end()) chosenPath = chosen->second;
            }
            if (!chosenPath.empty()) {
                if (current_variant(source, chosenPath)) {
                    AdoptVariant(source, mode, chosenPath);
                    return chosenPath;
                }
                std::lock_guard lock(mutex_);
                choices_.erase(key);
                leasedVariants_.erase(chosenPath);
            }
            if (current_variant(source, destination)) {
                AdoptVariant(source, mode, destination);
                append_log(dataRoot_, L"使用壁纸性能缓存: " + destination.filename().wstring());
                return destination;
            }
            if (reuse_equivalent_variant(source, destination, mode)) {
                AdoptVariant(source, mode, destination);
                append_log(dataRoot_, L"复用相同规格的壁纸性能副本: " + destination.filename().wstring());
                return destination;
            }
            {
                std::lock_guard lock(mutex_);
                if (generationAllowed_ && !on_battery() && failed_.find(key) == failed_.end() &&
                    (!active_ || active_->Key() != key) && !PendingContains(key)) {
                    pending_.push_back(Request{ source, destination, decision.targetFps,
                        dimensions.first, dimensions.second, mode,
                        generation_.load(std::memory_order_relaxed), false,
                        softwarePlaybackTarget ? rate.softwarePlaybackConversionAllowed :
                            rate.softwareFallbackAllowed,
                        softwarePlaybackTarget });
                    condition_.notify_one();
                }
            }
            // Never cache a fallback. Resolve must keep observing generation
            // eligibility and discover a completed target without another mode
            // toggle or process restart.
            return source;
        }

        void Prepare(fs::path const& source, std::string const& mode,
            uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate)
        {
            if (source.empty() || mode == "original" || !mediaFoundationStarted_) return;
            auto mediaDirectory = source.parent_path();
            if (fs::is_regular_file(motion::variant_cancelled_path(mediaDirectory))) return;
            if (motion::variant_generation_paused(mediaDirectory)) return;
            if (motion::variant_generation_suppressed(mediaDirectory, mode)) return;
            SourceRate rate;
            {
                std::lock_guard lock(mutex_);
                auto found = rates_.find(source.wstring());
                if (found != rates_.end()) rate = found->second;
            }
            if (!rate.numerator) {
                rate = source_rate(source);
                std::lock_guard lock(mutex_);
                rates_[source.wstring()] = rate;
            }
            auto dimensions = video_variant_dimensions(rate.width, rate.height, targetWidth, targetHeight);
            auto decision = video_variant_decision(mode, dimensions.first, dimensions.second,
                rate.numerator, rate.denominator, targetRefreshRate);
            if (!video_needs_variant(rate.numerator, rate.denominator, decision.targetFps,
                rate.width, rate.height, dimensions.first, dimensions.second)) {
                motion::complete_variant_generation(mediaDirectory, mode);
                return;
            }
            auto destination = mediaDirectory / L"Variants" / decision.fileName;
            if (current_variant(source, destination)) {
                AdoptVariant(source, mode, destination);
                motion::complete_variant_generation(mediaDirectory, mode);
                return;
            }
            if (reuse_equivalent_variant(source, destination, mode)) {
                AdoptVariant(source, mode, destination);
                motion::complete_variant_generation(mediaDirectory, mode);
                return;
            }
            auto key = source.wstring() + L"\n" + decision.fileName;
            std::lock_guard lock(mutex_);
            if (!generationAllowed_ || on_battery()) return;
            if (failed_.contains(key)) {
                motion::fail_variant_generation(mediaDirectory, mode);
                return;
            }
            if (active_ && active_->Key() == key) return;
            for (auto& pending : pending_) {
                if (pending.Key() != key) continue;
                pending.explicitRequest = true;
                return;
            }
            pending_.push_back(Request{ source, destination, decision.targetFps,
                dimensions.first, dimensions.second, mode,
                generation_.load(std::memory_order_relaxed), true,
                rate.softwareFallbackAllowed, false });
            condition_.notify_one();
        }

        void InvalidateChoices()
        {
            generation_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(mutex_);
            choices_.clear();
            failed_.clear();
            pending_.clear();
        }

        void SetGenerationAllowed(bool allowed)
        {
            std::lock_guard lock(mutex_);
            if (generationAllowed_ == allowed) return;
            generationAllowed_ = allowed;
            if (allowed) {
                choices_.clear();
            } else {
                generation_.fetch_add(1, std::memory_order_relaxed);
                pending_.clear();
            }
        }

    private:
        bool PendingContains(std::wstring const& key) const
        {
            return std::any_of(pending_.begin(), pending_.end(),
                [&](auto const& request) { return request.Key() == key; });
        }

        void AdoptVariant(fs::path const& source, std::string const& mode, fs::path const& destination)
        {
            bool shouldRetain{};
            auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(mutex_);
                auto prefix = variant_prefix(mode);
                for (auto choice = choices_.begin(); choice != choices_.end();) {
                    auto const& path = choice->second;
                    if (path != destination && path.parent_path() == destination.parent_path() &&
                        path.filename().wstring().starts_with(prefix)) {
                        leasedVariants_.erase(path);
                        choice = choices_.erase(choice);
                    } else {
                        ++choice;
                    }
                }
                choices_[source.wstring() + L"\n" + destination.filename().wstring()] = destination;
                leasedVariants_.insert(destination);
                if (!retainedProfiles_.contains(destination)) {
                    auto retry = retentionRetryAfter_.find(destination);
                    if (retry == retentionRetryAfter_.end() || now >= retry->second) {
                        retentionRetryAfter_[destination] = now + std::chrono::seconds(30);
                        shouldRetain = true;
                    }
                }
            }
            if (shouldRetain && motion::retain_variant_profile(
                source.parent_path(), mode, destination.filename().wstring())) {
                std::lock_guard lock(mutex_);
                retainedProfiles_.insert(destination);
                retentionRetryAfter_.erase(destination);
            }
        }

        void Run(std::stop_token stop)
        {
            init_apartment(apartment_type::multi_threaded);
            while (!stop.stop_requested()) {
                Request request;
                {
                    std::unique_lock lock(mutex_);
                    condition_.wait(lock, stop, [&] { return generationAllowed_ && !pending_.empty(); });
                    if (stop.stop_requested()) return;
                    request = std::move(pending_.front());
                    pending_.pop_front();
                    active_ = request;
                }
                auto transcodeResult = Transcode(request, stop);
                bool succeeded = transcodeResult == VideoTranscodeResult::succeeded;
                bool paused = transcodeResult == VideoTranscodeResult::paused ||
                    motion::variant_generation_paused(request.source.parent_path());
                bool obsolete = request.generation != generation_.load(std::memory_order_relaxed);
                bool cancelled = fs::is_regular_file(motion::variant_cancelled_path(request.source.parent_path()));
                bool suppressed = motion::variant_generation_suppressed(
                    request.source.parent_path(), request.mode);
                bool superseded = request.explicitRequest &&
                    motion::read_variant_request(request.source.parent_path()) != request.mode;
                bool accepted = succeeded && !paused && !obsolete && !cancelled && !suppressed && !superseded;
                {
                    std::lock_guard lock(mutex_);
                    active_.reset();
                    if (!succeeded && !paused && !stop.stop_requested() && !obsolete && !cancelled &&
                        !suppressed && !superseded) failed_.insert(request.Key());
                }
                if (accepted) {
                    AdoptVariant(request.source, request.mode, request.destination);
                    motion::complete_variant_generation(request.source.parent_path(), request.mode);
                } else if (succeeded) {
                    std::error_code ignored;
                    fs::remove(request.destination, ignored);
                }
                else if (!paused && request.explicitRequest && !stop.stop_requested() && !obsolete &&
                    !cancelled && !suppressed && !superseded) {
                    motion::fail_variant_generation(request.source.parent_path(), request.mode);
                }
                if (!paused && !obsolete && !cancelled && !suppressed && !superseded) {
                    append_log(dataRoot_, accepted
                        ? L"已生成壁纸优化副本: " + request.destination.filename().wstring()
                        : L"无法生成壁纸优化副本，继续使用原文件: " + request.source.filename().wstring());
                }
            }
        }

        VideoTranscodeResult Transcode(Request const& request, std::stop_token stop)
        {
            auto directory = request.destination.parent_path();
            auto temporary = directory / (request.destination.stem().wstring() + L".part.mp4");
            std::error_code ignored;
            try {
                fs::create_directories(directory);
                fs::remove(temporary, ignored);
                std::set<fs::path> protectedFiles;
                {
                    std::lock_guard lock(mutex_);
                    protectedFiles = leasedVariants_;
                }
                protectedFiles.insert(request.destination);
                protectedFiles.insert(temporary);
                prune_variant_cache(dataRoot_, protectedFiles);
                if (!has_transcode_space(directory, request.source)) {
                    append_log(dataRoot_, L"磁盘空间不足，跳过壁纸性能缓存生成。");
                    return VideoTranscodeResult::failed;
                }
                auto targetFps = request.targetFps;
                std::wstring error;
                std::wstring selectedBackend;
                auto result = transcode_video(
                    ffmpeg_,
                    request.source, temporary, request.width, request.height, targetFps,
                    [&] {
                        if (motion::variant_generation_paused(request.source.parent_path())) {
                            return VideoTranscodeControl::paused;
                        }
                        bool cancelled = stop.stop_requested() ||
                            request.generation != generation_.load(std::memory_order_relaxed) ||
                                fs::is_regular_file(motion::variant_cancelled_path(request.source.parent_path())) ||
                                motion::variant_generation_suppressed(request.source.parent_path(), request.mode) ||
                                (request.explicitRequest &&
                                motion::read_variant_request(request.source.parent_path()) != request.mode);
                        return cancelled ? VideoTranscodeControl::cancelled : VideoTranscodeControl::running;
                    }, error, &selectedBackend, request.softwareFallbackAllowed,
                    request.softwarePlaybackTarget);
                if (result == VideoTranscodeResult::cancelled || result == VideoTranscodeResult::paused) {
                    fs::remove(temporary, ignored);
                    return result;
                }
                if (result != VideoTranscodeResult::succeeded) {
                    append_log(dataRoot_, L"优化 " + std::to_wstring(targetFps) + L" FPS 不可用: " + error);
                    fs::remove(temporary, ignored);
                    return result;
                }
                if (!fs::is_regular_file(temporary) || !fs::file_size(temporary)) {
                    fs::remove(temporary, ignored);
                    return VideoTranscodeResult::failed;
                }
                auto actual = source_rate(temporary);
                // Container duration seldom ends exactly on a frame boundary,
                // so Media Foundation may report 59.94 for a CFR 60 stream.
                // One-frame-per-second tolerance accepts that representation
                // without accepting a different performance tier.
                bool matchingRate = video_variant_rate_matches(
                    actual.numerator, actual.denominator, targetFps);
                bool matchingDimensions = video_variant_dimensions_match(
                    actual.width, actual.height, request.width, request.height);
                bool matchingCodec = !request.softwarePlaybackTarget || actual.softwarePlaybackFriendly;
                if (!matchingDimensions || !matchingRate || !matchingCodec) {
                    append_log(dataRoot_, L"优化副本校验失败（实际 " +
                        std::to_wstring(actual.width) + L"x" + std::to_wstring(actual.height) + L", " +
                        std::to_wstring(actual.numerator) + L"/" + std::to_wstring(actual.denominator) +
                        L" FPS），已自动删除。");
                    fs::remove(temporary, ignored);
                    return VideoTranscodeResult::failed;
                }
                fs::remove(request.destination, ignored);
                fs::rename(temporary, request.destination);
                append_log(dataRoot_, L"优化副本实际帧率: " + std::to_wstring(targetFps) + L" FPS；编码后端: " +
                    (selectedBackend.empty() ? std::wstring(L"未知") : selectedBackend) + L"。");
                return current_variant(request.source, request.destination)
                    ? VideoTranscodeResult::succeeded : VideoTranscodeResult::failed;
            } catch (...) {
                fs::remove(temporary, ignored);
                return VideoTranscodeResult::failed;
            }
        }

        fs::path dataRoot_;
        fs::path ffmpeg_;
        std::mutex mutex_;
        std::condition_variable_any condition_;
        std::deque<Request> pending_;
        std::optional<Request> active_;
        std::map<std::wstring, SourceRate> rates_;
        std::map<std::wstring, fs::path> choices_;
        std::set<std::wstring> failed_;
        std::set<fs::path> leasedVariants_;
        std::set<fs::path> retainedProfiles_;
        std::map<fs::path, std::chrono::steady_clock::time_point> retentionRetryAfter_;
        std::jthread worker_;
        std::atomic_uint64_t generation_{};
        bool generationAllowed_{};
        bool mediaFoundationStarted_{};
    };

    VideoOptimizer::VideoOptimizer(fs::path dataRoot, fs::path applicationRoot)
        : impl_(std::make_unique<Impl>(std::move(dataRoot), std::move(applicationRoot))) {}
    VideoOptimizer::~VideoOptimizer() = default;
    fs::path VideoOptimizer::Resolve(fs::path const& source, std::string const& performanceMode,
        uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate,
        bool softwarePlaybackTarget)
    {
        return impl_->Resolve(source, performanceMode, targetWidth, targetHeight, targetRefreshRate,
            softwarePlaybackTarget);
    }
    void VideoOptimizer::Prepare(fs::path const& source, std::string const& performanceMode,
        uint32_t targetWidth, uint32_t targetHeight, uint32_t targetRefreshRate)
    {
        impl_->Prepare(source, performanceMode, targetWidth, targetHeight, targetRefreshRate);
    }
    void VideoOptimizer::SetGenerationAllowed(bool allowed) { impl_->SetGenerationAllowed(allowed); }
    void VideoOptimizer::InvalidateChoices() { impl_->InvalidateChoices(); }
}
