#pragma once

#include "UniqueHandle.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace motion
{
    struct VariantCacheFile
    {
        std::wstring fileName;
        std::string mode;
        uint64_t bytes{};
        bool sharedStorage{};
    };

    struct VariantCacheStatus
    {
        std::string requestedMode;
        bool queued{};
        bool generating{};
        bool paused{};
        bool cancelled{};
        bool failed{};
        std::string failedMode;
        bool balancedSuppressed{};
        bool powerSaverSuppressed{};
        uint64_t bytes{};
        uint32_t files{};
        std::vector<VariantCacheFile> entries;
    };

    [[nodiscard]] inline std::wstring select_variant_file(
        VariantCacheStatus const& status, std::string const& performanceMode) noexcept
    {
        auto rank = [&](VariantCacheFile const& entry) {
            if (performanceMode == "power-saver") return entry.mode == "power-saver" ? 0 : 1;
            // When the source has been removed, original mode cannot be honored.
            // Prefer the higher-quality retained profile, then fall back to the
            // power-saving copy so the wallpaper remains playable.
            return entry.mode == "balanced" ? 0 : 1;
        };
        VariantCacheFile const* best{};
        for (auto const& entry : status.entries) {
            if (entry.fileName.empty() || !entry.bytes) continue;
            if (!best || rank(entry) < rank(*best) ||
                (rank(entry) == rank(*best) &&
                    entry.fileName.ends_with(L"-v4.mp4") && !best->fileName.ends_with(L"-v4.mp4"))) {
                best = &entry;
            }
        }
        return best ? best->fileName : std::wstring{};
    }

    // CPU-smooth copies are an internal compatibility cache rather than a
    // user-selected quality tier. Keep them out of the variants UI, but allow
    // a source-less wallpaper to remain playable on a WARP-only system.
    [[nodiscard]] inline std::wstring select_cpu_smooth_variant_file(
        std::filesystem::path const& mediaDirectory) noexcept
    {
        try {
            std::wstring best;
            std::filesystem::file_time_type bestTime{};
            std::error_code error;
            for (std::filesystem::directory_iterator entries(mediaDirectory / L"Variants", error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError || !entries->file_size(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (!name.starts_with(L"cpu-smooth-") || !name.ends_with(L"-v5.mp4")) continue;
                auto modified = entries->last_write_time(itemError);
                if (itemError) continue;
                if (best.empty() || modified > bestTime) {
                    best = std::move(name);
                    bestTime = modified;
                }
            }
            return best;
        } catch (...) {
            return {};
        }
    }

    inline std::filesystem::path variant_request_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-request";
    }

    inline std::filesystem::path variant_cancelled_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-cancelled";
    }

    inline std::filesystem::path variant_paused_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-paused";
    }

    inline std::filesystem::path variant_failed_path(std::filesystem::path const& mediaDirectory)
    {
        return mediaDirectory / L".optimization-failed";
    }

    inline std::filesystem::path variant_suppressed_path(std::filesystem::path const& mediaDirectory,
        std::string const& mode)
    {
        return mediaDirectory / (mode == "power-saver"
            ? L".optimization-suppressed-power-saver"
            : L".optimization-suppressed-balanced");
    }

    inline bool variant_generation_suppressed(std::filesystem::path const& mediaDirectory,
        std::string const& mode) noexcept
    {
        if (mode != "balanced" && mode != "power-saver") return false;
        std::error_code error;
        return std::filesystem::is_regular_file(variant_suppressed_path(mediaDirectory, mode), error) && !error;
    }

    inline bool retain_variant_profile(std::filesystem::path const& mediaDirectory,
        std::string const& mode, std::wstring const& keepFileName) noexcept
    {
        if ((mode != "balanced" && mode != "power-saver" && mode != "cpu-smooth") || keepFileName.empty()) return false;
        try {
            auto variants = mediaDirectory / L"Variants";
            auto prefix = mode == "balanced" ? L"balanced-" :
                mode == "power-saver" ? L"power-saver-" : L"cpu-smooth-";
            if (!keepFileName.starts_with(prefix) || !keepFileName.ends_with(L".mp4") ||
                keepFileName.ends_with(L".part.mp4")) return false;
            std::error_code error;
            if (!std::filesystem::exists(variants, error)) return !error;
            if (!std::filesystem::is_regular_file(variants / keepFileName, error) || error) return false;
            bool retainedOnlyCurrent = true;
            for (std::filesystem::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                auto regular = entries->is_regular_file(itemError);
                if (itemError) {
                    retainedOnlyCurrent = false;
                    continue;
                }
                if (!regular) continue;
                auto name = entries->path().filename().wstring();
                if (!name.starts_with(prefix) || name == keepFileName ||
                    name.ends_with(L".part.mp4") || entries->path().extension() != L".mp4") continue;
                if (!std::filesystem::remove(entries->path(), itemError) || itemError) {
                    retainedOnlyCurrent = false;
                }
            }
            return !error && retainedOnlyCurrent;
        } catch (...) {
            return false;
        }
    }

    inline bool write_small_file(std::filesystem::path const& destination, std::string const& value) noexcept
    {
        try {
            auto temporary = destination;
            temporary += L".tmp";
            unique_handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr));
            if (!file || value.size() > MAXDWORD) return false;
            DWORD written{};
            if (!WriteFile(file.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr) ||
                written != static_cast<DWORD>(value.size()) || !FlushFileBuffers(file.get())) {
                file.reset();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            file.reset();
            if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    inline std::string read_variant_request(std::filesystem::path const& mediaDirectory) noexcept
    {
        try {
            auto path = variant_request_path(mediaDirectory);
            unique_handle file(CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) return {};
            char value[32]{};
            DWORD read{};
            if (!ReadFile(file.get(), value, sizeof(value) - 1, &read, nullptr)) return {};
            std::string result(value, value + read);
            return result == "balanced" || result == "power-saver" ? result : std::string{};
        } catch (...) {
            return {};
        }
    }

    inline bool request_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& mode) noexcept
    {
        if (mode != "balanced" && mode != "power-saver") return false;
        try {
            std::filesystem::create_directories(mediaDirectory);
            std::error_code ignored;
            std::filesystem::remove(variant_cancelled_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_suppressed_path(mediaDirectory, mode), ignored);
            return write_small_file(variant_request_path(mediaDirectory), mode);
        } catch (...) {
            return false;
        }
    }

    inline bool variant_generation_paused(std::filesystem::path const& mediaDirectory) noexcept
    {
        std::error_code error;
        return std::filesystem::is_regular_file(variant_paused_path(mediaDirectory), error) && !error;
    }

    inline bool pause_variant_generation(std::filesystem::path const& mediaDirectory) noexcept
    {
        if (read_variant_request(mediaDirectory).empty()) return false;
        return write_small_file(variant_paused_path(mediaDirectory), "paused");
    }

    inline bool resume_variant_generation(std::filesystem::path const& mediaDirectory) noexcept
    {
        if (read_variant_request(mediaDirectory).empty()) return false;
        std::error_code error;
        std::filesystem::remove(variant_paused_path(mediaDirectory), error);
        return !error;
    }

    inline void remove_variant_partials(std::filesystem::path const& mediaDirectory) noexcept
    {
        try {
            std::error_code error;
            auto variants = mediaDirectory / L"Variants";
            for (std::filesystem::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (name.ends_with(L".part.mp4")) std::filesystem::remove(entries->path(), itemError);
            }
        } catch (...) {}
    }

    inline bool suppress_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& mode) noexcept
    {
        if (mode != "balanced" && mode != "power-saver") return false;
        if (!write_small_file(variant_suppressed_path(mediaDirectory, mode), "suppressed")) return false;
        if (read_variant_request(mediaDirectory) == mode) {
            std::error_code ignored;
            std::filesystem::remove(variant_request_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
        }
        return true;
    }

    inline bool cancel_variant_generation(std::filesystem::path const& mediaDirectory) noexcept
    {
        std::error_code ignored;
        if (!write_small_file(variant_cancelled_path(mediaDirectory), "cancelled")) return false;
        std::filesystem::remove(variant_request_path(mediaDirectory), ignored);
        std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
        std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
        remove_variant_partials(mediaDirectory);
        return true;
    }

    inline void complete_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& expectedMode = {}) noexcept
    {
        std::error_code ignored;
        if (!expectedMode.empty() && read_variant_request(mediaDirectory) != expectedMode) return;
        std::filesystem::remove(variant_request_path(mediaDirectory), ignored);
        std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
        std::filesystem::remove(variant_failed_path(mediaDirectory), ignored);
    }

    inline void fail_variant_generation(std::filesystem::path const& mediaDirectory,
        std::string const& expectedMode = {}) noexcept
    {
        std::error_code ignored;
        if (!expectedMode.empty() && read_variant_request(mediaDirectory) != expectedMode) return;
        auto failedMode = expectedMode.empty() ? read_variant_request(mediaDirectory) : expectedMode;
        if (failedMode != "balanced" && failedMode != "power-saver") failedMode = "failed";
        if (write_small_file(variant_failed_path(mediaDirectory), failedMode)) {
            std::filesystem::remove(variant_request_path(mediaDirectory), ignored);
            std::filesystem::remove(variant_paused_path(mediaDirectory), ignored);
        }
    }

    inline VariantCacheStatus inspect_variant_cache(std::filesystem::path const& mediaDirectory) noexcept
    {
        VariantCacheStatus result;
        try {
            result.requestedMode = read_variant_request(mediaDirectory);
            result.queued = !result.requestedMode.empty();
            result.paused = result.queued && variant_generation_paused(mediaDirectory);
            result.cancelled = std::filesystem::is_regular_file(variant_cancelled_path(mediaDirectory));
            result.failed = std::filesystem::is_regular_file(variant_failed_path(mediaDirectory));
            if (result.failed) {
                unique_handle failure(CreateFileW(variant_failed_path(mediaDirectory).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
                char value[32]{};
                DWORD read{};
                if (failure && ReadFile(failure.get(), value, sizeof(value) - 1, &read, nullptr)) {
                    std::string mode(value, value + read);
                    if (mode == "balanced" || mode == "power-saver") result.failedMode = std::move(mode);
                }
            }
            result.balancedSuppressed = variant_generation_suppressed(mediaDirectory, "balanced");
            result.powerSaverSuppressed = variant_generation_suppressed(mediaDirectory, "power-saver");
            auto variants = mediaDirectory / L"Variants";
            std::error_code error;
            for (std::filesystem::directory_iterator entries(variants, error), end;
                !error && entries != end; entries.increment(error)) {
                std::error_code itemError;
                if (!entries->is_regular_file(itemError) || itemError) continue;
                auto name = entries->path().filename().wstring();
                if (name.ends_with(L".part.mp4")) result.generating = true;
                else if (entries->path().extension() == L".mp4") {
                    auto size = entries->file_size(itemError);
                    if (!itemError) {
                        std::string mode;
                        if (name.starts_with(L"balanced-")) mode = "balanced";
                        else if (name.starts_with(L"power-saver-")) mode = "power-saver";
                        if (mode.empty()) continue;
                        result.entries.push_back({ name, std::move(mode), size });
                    }
                }
            }
            struct FileIdentity
            {
                DWORD volume{};
                DWORD high{};
                DWORD low{};
            };
            std::vector<std::pair<FileIdentity, size_t>> identities;
            for (size_t index = 0; index < result.entries.size(); ++index) {
                auto const path = variants / result.entries[index].fileName;
                unique_handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                BY_HANDLE_FILE_INFORMATION information{};
                bool identified = file && GetFileInformationByHandle(file.get(), &information);
                FileIdentity identity{ information.dwVolumeSerialNumber,
                    information.nFileIndexHigh, information.nFileIndexLow };
                auto duplicate = identified ? std::find_if(identities.begin(), identities.end(),
                    [&](auto const& existing) {
                        return existing.first.volume == identity.volume &&
                            existing.first.high == identity.high && existing.first.low == identity.low;
                    }) : identities.end();
                if (duplicate != identities.end()) {
                    result.entries[index].sharedStorage = true;
                    result.entries[duplicate->second].sharedStorage = true;
                    continue;
                }
                if (identified) identities.emplace_back(identity, index);
                ++result.files;
                result.bytes += result.entries[index].bytes;
            }
        } catch (...) {}
        return result;
    }
}
