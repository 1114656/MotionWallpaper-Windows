#include "Common.h"
#include "TextEncoding.h"
#include "UniqueHandle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

namespace fs = std::filesystem;
using namespace winrt;
using namespace Windows::Data::Json;

namespace
{
    bool safe_display_id(std::string const& value)
    {
        return !value.empty() && value.size() <= 512 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x20 && character != 0x7f;
        });
    }

    std::wstring read_text(fs::path const& path)
    {
        std::error_code sizeError;
        auto size = fs::file_size(path, sizeError);
        if (!sizeError && size > 4 * 1024 * 1024) throw std::runtime_error("JSON file is too large");
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return motion::utf8_to_wide(bytes);
    }

    void write_text_atomic(fs::path const& path, std::wstring const& value)
    {
        fs::create_directories(path.parent_path());
        auto identity = fs::absolute(path).lexically_normal().wstring();
        uint64_t hash = 1469598103934665603ULL;
        for (auto character : identity) {
            hash ^= static_cast<uint16_t>(towlower(character));
            hash *= 1099511628211ULL;
        }
        auto mutexName = L"Local\\MotionWallpaper.AtomicJson." + std::to_wstring(hash);
        motion::unique_handle writeMutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
        if (!writeMutex) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        auto waitResult = WaitForSingleObject(writeMutex.get(), 10'000);
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
            throw std::runtime_error("timed out waiting for atomic JSON writer");
        }
        struct MutexRelease
        {
            HANDLE value{};
            ~MutexRelease() { if (value) ReleaseMutex(value); }
        } release{ writeMutex.get() };
        static std::atomic_uint64_t temporarySequence{};
        auto temporary = path.parent_path() /
            (path.filename().wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(++temporarySequence));
        try {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot open temporary JSON file: " + motion::wide_to_utf8(temporary.wstring()));
            auto bytes = motion::wide_to_utf8(value);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) throw std::runtime_error("cannot write temporary JSON file");
            output.close();
            DWORD replaceError{};
            bool replaced{};
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    replaced = true;
                    break;
                }
                replaceError = GetLastError();
                if (replaceError != ERROR_ACCESS_DENIED && replaceError != ERROR_SHARING_VIOLATION &&
                    replaceError != ERROR_LOCK_VIOLATION) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2u << attempt));
            }
            if (!replaced) throw std::system_error(static_cast<int>(replaceError), std::system_category());
        } catch (...) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
    }

    JsonObject parse_object(fs::path const& path)
    {
        auto text = read_text(path);
        if (text.empty()) throw std::runtime_error("JSON file is empty");
        return JsonObject::Parse(text);
    }

    std::wstring json_wstring(JsonObject const& object, wchar_t const* name)
    {
        return object.GetNamedString(name, L"").c_str();
    }

    std::string json_string(JsonObject const& object, wchar_t const* name)
    {
        return motion::wide_to_utf8(json_wstring(object, name));
    }

    int json_int(JsonObject const& object, wchar_t const* name, int fallback, int minimum, int maximum)
    {
        auto value = object.GetNamedNumber(name, fallback);
        if (!std::isfinite(value)) return fallback;
        value = (std::clamp)(value, static_cast<double>(minimum), static_cast<double>(maximum));
        return static_cast<int>(value);
    }

    uint64_t json_uint64(JsonObject const& object, wchar_t const* name)
    {
        auto value = object.GetNamedNumber(name, 0);
        if (!std::isfinite(value) || value <= 0) return 0;
        // JSON numbers are doubles. UINT64_MAX rounds up to 2^64 and would make
        // the final conversion undefined, so cap at an exactly convertible bound.
        constexpr auto maximum = static_cast<double>((std::numeric_limits<int64_t>::max)());
        return static_cast<uint64_t>((std::min)(value, maximum));
    }
}

namespace motion
{
    void append_utf8_log(fs::path const& path, std::wstring_view message) noexcept
    {
        try {
            if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
            constexpr uintmax_t maximumLogBytes = 2 * 1024 * 1024;
            constexpr uintmax_t retainedLogBytes = maximumLogBytes / 2;
            std::error_code sizeError;
            auto size = fs::file_size(path, sizeError);
            if (!sizeError && size > maximumLogBytes) {
                std::ifstream input(path, std::ios::binary);
                if (input) {
                    input.seekg(-static_cast<std::streamoff>(retainedLogBytes), std::ios::end);
                    std::string retained((std::istreambuf_iterator<char>(input)), {});
                    auto firstLine = retained.find('\n');
                    if (firstLine != std::string::npos) retained.erase(0, firstLine + 1);
                    auto temporary = path;
                    temporary += L".rotate.tmp";
                    std::ofstream rotated(temporary, std::ios::binary | std::ios::trunc);
                    if (rotated) {
                        rotated.write(retained.data(), static_cast<std::streamsize>(retained.size()));
                        rotated.close();
                        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                    }
                }
            }
            auto line = wide_to_utf8(timestamp_utc() + L" " + std::wstring(message) + L"\n");
            std::ofstream output(path, std::ios::binary | std::ios::app);
            if (output) output.write(line.data(), static_cast<std::streamsize>(line.size()));
        } catch (...) {}
    }

    std::wstring quote_command_line_argument(std::wstring_view value)
    {
        if (value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(value);
        std::wstring result{ L'\"' };
        size_t backslashes{};
        for (auto character : value) {
            if (character == L'\\') {
                ++backslashes;
            } else if (character == L'\"') {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(character);
                backslashes = 0;
            } else {
                result.append(backslashes, L'\\');
                result.push_back(character);
                backslashes = 0;
            }
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::wstring build_command_line(std::vector<std::wstring> const& arguments)
    {
        std::wstring result;
        for (auto const& argument : arguments) {
            if (!result.empty()) result.push_back(L' ');
            result += quote_command_line_argument(argument);
        }
        return result;
    }

    std::chrono::milliseconds IdleTimer::Update(
        std::chrono::milliseconds now,
        uint32_t inputTick,
        std::chrono::milliseconds rawIdle,
        bool activityInhibitsIdle) noexcept
    {
        if (!initialized_ || inputTick != inputTick_) {
            initialized_ = true;
            inputTick_ = inputTick;
            idleSince_ = now - std::min(now, rawIdle);
        }
        if (activityInhibitsIdle) idleSince_ = now;
        return now > idleSince_ ? now - idleSince_ : std::chrono::milliseconds::zero();
    }

    fs::path executable_directory()
    {
        std::wstring value(32768, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
        if (!length || length >= value.size()) throw std::runtime_error("GetModuleFileNameW failed");
        value.resize(length);
        return fs::path(value).parent_path();
    }

    fs::path select_application_data_directory(fs::path const& applicationRoot, fs::path const& localAppDataRoot)
    {
        std::error_code error;
        bool portable = fs::is_regular_file(applicationRoot / L"portable.mode", error);
        error.clear();
        bool legacyConfig = fs::is_directory(applicationRoot / L"Config", error);
        error.clear();
        bool legacyLibrary = fs::is_directory(applicationRoot / L"Wallpapers", error);
        if (portable || legacyConfig || legacyLibrary || localAppDataRoot.empty()) return applicationRoot;
        return localAppDataRoot / L"MotionWallpaper";
    }

    fs::path application_data_directory()
    {
        auto applicationRoot = executable_directory();
        DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (!required) return select_application_data_directory(applicationRoot, {});
        std::wstring value(required, L'\0');
        DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (!length || length >= required) return select_application_data_directory(applicationRoot, {});
        value.resize(length);
        return select_application_data_directory(applicationRoot, fs::path(value));
    }

    std::wstring utf8_to_wide(std::string const& value)
    {
        if (value.empty()) return {};
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0) throw std::runtime_error("invalid UTF-8");
        std::wstring result(static_cast<size_t>(length), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length)) {
            throw std::runtime_error("UTF-8 conversion failed");
        }
        return result;
    }

    std::string wide_to_utf8(std::wstring_view value)
    {
        return utf8_from_wide(value);
    }

    std::string new_id()
    {
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("CoCreateGuid failed");
        wchar_t value[40]{};
        if (StringFromGUID2(id, value, ARRAYSIZE(value)) <= 0) throw std::runtime_error("GUID conversion failed");
        std::wstring result(value + 1, value + 37);
        std::transform(result.begin(), result.end(), result.begin(), towlower);
        return wide_to_utf8(result);
    }

    std::wstring timestamp_utc()
    {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        wchar_t value[40]{};
        swprintf_s(value, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        return value;
    }

    bool valid_id(std::string const& value)
    {
        return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || character == '-';
        });
    }

    bool valid_id(std::wstring const& value)
    {
        return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](wchar_t character) {
            return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') || character == L'-';
        });
    }

    bool safe_file_name(fs::path const& value)
    {
        return !value.empty() && !value.is_absolute() && value == value.filename() &&
            value != L"." && value != L"..";
    }

    std::optional<Settings> load_settings(fs::path const& path)
    {
        if (!fs::is_regular_file(path)) return std::nullopt;
        auto object = parse_object(path);
        Settings settings;
        auto storedVersion = json_int(object, L"version", 1, 0, settings_schema_version + 1);
        if (storedVersion < 1 || storedVersion > settings_schema_version) return std::nullopt;
        settings.version = settings_schema_version;
        settings.desktopPlayback = object.GetNamedBoolean(L"desktopPlayback", settings.desktopPlayback);
        settings.activePlaybackEnabled = object.GetNamedBoolean(L"activePlaybackEnabled", settings.activePlaybackEnabled);
        settings.continueWhenCovered = object.GetNamedBoolean(L"continueWhenCovered", settings.continueWhenCovered);
        settings.screensaverEnabled = object.GetNamedBoolean(L"screensaverEnabled", settings.screensaverEnabled);
        settings.idleTimeoutSeconds = json_int(object, L"idleTimeoutSeconds", settings.idleTimeoutSeconds, 10, 86400);
        if (storedVersion >= 8) {
            settings.autoLockEnabled = object.GetNamedBoolean(L"autoLockEnabled", settings.autoLockEnabled);
            settings.autoLockTimeoutSeconds = json_int(
                object, L"autoLockTimeoutSeconds", settings.autoLockTimeoutSeconds, 30, 86400);
            settings.displayOffAfterLockEnabled = object.GetNamedBoolean(
                L"displayOffAfterLockEnabled", settings.displayOffAfterLockEnabled);
            settings.displayOffAfterLockDelaySeconds = json_int(
                object, L"displayOffAfterLockDelaySeconds", settings.displayOffAfterLockDelaySeconds, 0, 86400);
        } else if (storedVersion == 7) {
            settings.autoLockEnabled = object.GetNamedBoolean(L"displayOffEnabled", settings.autoLockEnabled);
            settings.autoLockTimeoutSeconds = json_int(
                object, L"displayOffTimeoutSeconds", settings.autoLockTimeoutSeconds, 30, 86400);
            settings.displayOffAfterLockEnabled = settings.autoLockEnabled;
            settings.displayOffAfterLockDelaySeconds = 30;
        } else {
            settings.autoLockEnabled = object.GetNamedBoolean(L"autoLockEnabled", settings.autoLockEnabled);
            settings.autoLockTimeoutSeconds = json_int(
                object, L"lockTimeoutSeconds", settings.autoLockTimeoutSeconds, 30, 86400);
            settings.displayOffAfterLockEnabled = false;
        }
        settings.decodeMode = json_string(object, L"decodeMode");
        if (settings.decodeMode != "auto" && settings.decodeMode != "hardware" && settings.decodeMode != "software") settings.decodeMode = "auto";
        settings.performanceMode = json_string(object, L"performanceMode");
        if (settings.performanceMode != "balanced" && settings.performanceMode != "original" &&
            settings.performanceMode != "power-saver") settings.performanceMode = "balanced";
        settings.selectedGroupId = json_string(object, L"selectedGroupId");
        settings.selectedMediaId = object.HasKey(L"selectedMediaId")
            ? json_string(object, L"selectedMediaId")
            : json_string(object, L"selectedVideoId");
        settings.randomGroupId = json_string(object, L"randomGroupId");
        settings.randomIntervalMinutes = json_int(object, L"randomIntervalMinutes", 0, 0, 1440);
        settings.startWithWindows = object.GetNamedBoolean(L"startWithWindows", false);
        settings.displayMode = json_string(object, L"displayMode");
        if (settings.displayMode == "span") settings.displayMode = "independent";
        if (settings.displayMode != "independent" && settings.displayMode != "primary") settings.displayMode = "independent";
        if (object.HasKey(L"displayAssignments")) {
            auto assignments = object.GetNamedArray(L"displayAssignments");
            for (auto const& value : assignments) {
                if (value.ValueType() != JsonValueType::Object) continue;
                auto assignmentObject = value.GetObject();
                DisplayAssignment assignment{
                    json_string(assignmentObject, L"displayId"),
                    json_string(assignmentObject, L"groupId"),
                    json_string(assignmentObject, L"mediaId")
                };
                if (!safe_display_id(assignment.displayId) || !valid_id(assignment.groupId) || !valid_id(assignment.mediaId)) continue;
                auto duplicate = std::find_if(settings.displayAssignments.begin(), settings.displayAssignments.end(),
                    [&](auto const& existing) { return existing.displayId == assignment.displayId; });
                if (duplicate == settings.displayAssignments.end()) settings.displayAssignments.push_back(std::move(assignment));
            }
        }
        return settings;
    }

    bool try_load_settings(fs::path const& path, Settings& destination) noexcept
    {
        try {
            auto loaded = load_settings(path);
            if (!loaded) return false;
            destination = std::move(*loaded);
            return true;
        } catch (...) {
            return false;
        }
    }

    void save_settings(fs::path const& path, Settings const& settings)
    {
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(settings_schema_version));
        object.Insert(L"desktopPlayback", JsonValue::CreateBooleanValue(settings.desktopPlayback));
        object.Insert(L"activePlaybackEnabled", JsonValue::CreateBooleanValue(settings.activePlaybackEnabled));
        object.Insert(L"continueWhenCovered", JsonValue::CreateBooleanValue(settings.continueWhenCovered));
        object.Insert(L"screensaverEnabled", JsonValue::CreateBooleanValue(settings.screensaverEnabled));
        object.Insert(L"idleTimeoutSeconds", JsonValue::CreateNumberValue(settings.idleTimeoutSeconds));
        object.Insert(L"autoLockEnabled", JsonValue::CreateBooleanValue(settings.autoLockEnabled));
        object.Insert(L"autoLockTimeoutSeconds", JsonValue::CreateNumberValue(settings.autoLockTimeoutSeconds));
        object.Insert(L"displayOffAfterLockEnabled", JsonValue::CreateBooleanValue(settings.displayOffAfterLockEnabled));
        object.Insert(L"displayOffAfterLockDelaySeconds", JsonValue::CreateNumberValue(settings.displayOffAfterLockDelaySeconds));
        object.Insert(L"decodeMode", JsonValue::CreateStringValue(utf8_to_wide(settings.decodeMode)));
        object.Insert(L"performanceMode", JsonValue::CreateStringValue(utf8_to_wide(settings.performanceMode)));
        object.Insert(L"selectedGroupId", JsonValue::CreateStringValue(utf8_to_wide(settings.selectedGroupId)));
        object.Insert(L"selectedMediaId", JsonValue::CreateStringValue(utf8_to_wide(settings.selectedMediaId)));
        object.Insert(L"randomGroupId", JsonValue::CreateStringValue(utf8_to_wide(settings.randomGroupId)));
        object.Insert(L"randomIntervalMinutes", JsonValue::CreateNumberValue(settings.randomIntervalMinutes));
        object.Insert(L"startWithWindows", JsonValue::CreateBooleanValue(settings.startWithWindows));
        object.Insert(L"displayMode", JsonValue::CreateStringValue(utf8_to_wide(settings.displayMode)));
        JsonArray assignments;
        for (auto const& assignment : settings.displayAssignments) {
            if (!safe_display_id(assignment.displayId) || !valid_id(assignment.groupId) || !valid_id(assignment.mediaId)) continue;
            JsonObject value;
            value.Insert(L"displayId", JsonValue::CreateStringValue(utf8_to_wide(assignment.displayId)));
            value.Insert(L"groupId", JsonValue::CreateStringValue(utf8_to_wide(assignment.groupId)));
            value.Insert(L"mediaId", JsonValue::CreateStringValue(utf8_to_wide(assignment.mediaId)));
            assignments.Append(value);
        }
        object.Insert(L"displayAssignments", assignments);
        write_text_atomic(path, object.Stringify().c_str());
    }

    std::optional<RuntimeState> load_runtime(fs::path const& path)
    {
        if (!fs::is_regular_file(path)) return std::nullopt;
        auto object = parse_object(path);
        RuntimeState runtime;
        auto version = json_int(object, L"version", runtime_schema_version, 0, runtime_schema_version + 1);
        if (version != runtime_schema_version) return std::nullopt;
        runtime.activeGroupId = json_string(object, L"activeGroupId");
        runtime.activeMediaId = json_string(object, L"activeMediaId");
        runtime.decodePath = json_string(object, L"decodePath");
        runtime.decodeReason = json_string(object, L"decodeReason");
        runtime.updatedAt = json_wstring(object, L"updatedAt");
        if ((!runtime.activeGroupId.empty() && !valid_id(runtime.activeGroupId)) ||
            (!runtime.activeMediaId.empty() && !valid_id(runtime.activeMediaId))) return std::nullopt;
        if (runtime.activeGroupId.empty() != runtime.activeMediaId.empty()) return std::nullopt;
        if (runtime.decodePath != "" && runtime.decodePath != "probing" &&
            runtime.decodePath != "hardware" && runtime.decodePath != "software" &&
            runtime.decodePath != "software-fallback" && runtime.decodePath != "unavailable" &&
            runtime.decodePath != "not-applicable") return std::nullopt;
        return runtime;
    }

    bool try_load_runtime(fs::path const& path, RuntimeState& destination) noexcept
    {
        try {
            auto loaded = load_runtime(path);
            if (!loaded) return false;
            destination = std::move(*loaded);
            return true;
        } catch (...) {
            return false;
        }
    }

    void save_runtime(fs::path const& path, RuntimeState const& runtime)
    {
        if ((!runtime.activeGroupId.empty() && !valid_id(runtime.activeGroupId)) ||
            (!runtime.activeMediaId.empty() && !valid_id(runtime.activeMediaId)) ||
            runtime.activeGroupId.empty() != runtime.activeMediaId.empty()) {
            throw std::runtime_error("invalid runtime state");
        }
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(runtime_schema_version));
        object.Insert(L"activeGroupId", JsonValue::CreateStringValue(utf8_to_wide(runtime.activeGroupId)));
        object.Insert(L"activeMediaId", JsonValue::CreateStringValue(utf8_to_wide(runtime.activeMediaId)));
        object.Insert(L"decodePath", JsonValue::CreateStringValue(utf8_to_wide(runtime.decodePath)));
        object.Insert(L"decodeReason", JsonValue::CreateStringValue(utf8_to_wide(runtime.decodeReason)));
        object.Insert(L"updatedAt", JsonValue::CreateStringValue(runtime.updatedAt));
        write_text_atomic(path, object.Stringify().c_str());
    }

    std::optional<GroupMetadata> load_group(fs::path const& path)
    {
        if (!fs::is_regular_file(path)) return std::nullopt;
        auto object = parse_object(path);
        GroupMetadata group;
        group.version = json_int(object, L"version", group_schema_version, 0, group_schema_version + 1);
        group.id = json_string(object, L"id");
        group.name = json_wstring(object, L"name");
        group.order = json_int(object, L"order", 0, 0, 1'000'000);
        group.createdAt = json_wstring(object, L"createdAt");
        group.updatedAt = json_wstring(object, L"updatedAt");
        if (group.version != group_schema_version || !valid_id(group.id) || group.name.empty()) return std::nullopt;
        return group;
    }

    void save_group(fs::path const& path, GroupMetadata const& group)
    {
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(group.version));
        object.Insert(L"id", JsonValue::CreateStringValue(utf8_to_wide(group.id)));
        object.Insert(L"name", JsonValue::CreateStringValue(group.name));
        object.Insert(L"order", JsonValue::CreateNumberValue(group.order));
        object.Insert(L"createdAt", JsonValue::CreateStringValue(group.createdAt));
        object.Insert(L"updatedAt", JsonValue::CreateStringValue(group.updatedAt));
        write_text_atomic(path, object.Stringify().c_str());
    }

    std::optional<MediaMetadata> load_media(fs::path const& path)
    {
        if (!fs::is_regular_file(path)) return std::nullopt;
        auto object = parse_object(path);
        MediaMetadata media;
        media.version = json_int(object, L"version", media_schema_version, 0, media_schema_version + 1);
        media.id = json_string(object, L"id");
        media.groupId = json_string(object, L"groupId");
        media.name = json_wstring(object, L"name");
        media.originalName = json_wstring(object, L"originalName");
        media.fileName = json_wstring(object, L"fileName");
        media.kind = json_string(object, L"kind");
        if (media.kind.empty()) media.kind = "video";
        media.coverFileName = json_wstring(object, L"coverFileName");
        media.sha256 = json_wstring(object, L"sha256");
        media.sizeBytes = json_uint64(object, L"sizeBytes");
        media.revision = json_uint64(object, L"revision");
        media.importedAt = json_wstring(object, L"importedAt");
        media.updatedAt = json_wstring(object, L"updatedAt");
        if (media.version != media_schema_version || !valid_id(media.id) || !valid_id(media.groupId) || !safe_file_name(media.fileName) ||
            (!media.coverFileName.empty() && !safe_file_name(media.coverFileName)) ||
            (media.kind != "video" && media.kind != "image")) return std::nullopt;
        return media;
    }

    bool try_load_media(fs::path const& path, MediaMetadata& destination) noexcept
    {
        try {
            auto loaded = load_media(path);
            if (!loaded) return false;
            destination = std::move(*loaded);
            return true;
        } catch (...) {
            return false;
        }
    }

    void save_media(fs::path const& path, MediaMetadata const& media)
    {
        if (!valid_id(media.id) || !valid_id(media.groupId) || !safe_file_name(media.fileName) ||
            (!media.coverFileName.empty() && !safe_file_name(media.coverFileName)) ||
            (media.kind != "video" && media.kind != "image")) {
            throw std::runtime_error("invalid media metadata");
        }
        JsonObject object;
        object.Insert(L"version", JsonValue::CreateNumberValue(media.version));
        object.Insert(L"id", JsonValue::CreateStringValue(utf8_to_wide(media.id)));
        object.Insert(L"groupId", JsonValue::CreateStringValue(utf8_to_wide(media.groupId)));
        object.Insert(L"name", JsonValue::CreateStringValue(media.name));
        object.Insert(L"kind", JsonValue::CreateStringValue(utf8_to_wide(media.kind)));
        object.Insert(L"originalName", JsonValue::CreateStringValue(media.originalName));
        object.Insert(L"fileName", JsonValue::CreateStringValue(media.fileName));
        object.Insert(L"coverFileName", JsonValue::CreateStringValue(media.coverFileName));
        object.Insert(L"sha256", JsonValue::CreateStringValue(media.sha256));
        object.Insert(L"sizeBytes", JsonValue::CreateNumberValue(static_cast<double>(media.sizeBytes)));
        object.Insert(L"revision", JsonValue::CreateNumberValue(static_cast<double>(media.revision)));
        object.Insert(L"importedAt", JsonValue::CreateStringValue(media.importedAt));
        object.Insert(L"updatedAt", JsonValue::CreateStringValue(media.updatedAt));
        write_text_atomic(path, object.Stringify().c_str());
    }

    DesktopIntent desktop_intent(Settings const& settings, bool covered, bool hasMedia)
    {
        if (!settings.desktopPlayback || !hasMedia) return DesktopIntent::Off;
        if (covered && !settings.continueWhenCovered) return DesktopIntent::Pause;
        return settings.activePlaybackEnabled ? DesktopIntent::Play : DesktopIntent::Freeze;
    }

    bool notify_settings_changed()
    {
        unique_handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, settings_event_name));
        return event && SetEvent(event.get());
    }
}
