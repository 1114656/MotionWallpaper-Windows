#include <windows.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <powrprof.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <wrl.h>

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/DisplayAwareness.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Common/VariantCache.h"
#include "CoveragePolicy.h"
#include "IdlePolicy.h"
#include "RandomSelectionPolicy.h"
#include "RuntimePolicy.h"
#include "SharedRendererPolicy.h"
#include "VideoOptimizer.h"
#include "../MotionWallpaper.Protocol/RendererProtocol.h"

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
    std::wstring display_adapter_key(std::wstring const& displayDevice)
    {
        if (displayDevice.empty()) return {};
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
        for (UINT adapterIndex = 0;; ++adapterIndex) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 adapterDescription{};
            if (FAILED(adapter->GetDesc1(&adapterDescription))) continue;
            for (UINT outputIndex = 0;; ++outputIndex) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC outputDescription{};
                if (FAILED(output->GetDesc(&outputDescription)) ||
                    _wcsicmp(outputDescription.DeviceName, displayDevice.c_str()) != 0) continue;
                return std::to_wstring(adapterDescription.AdapterLuid.HighPart) + L":" +
                    std::to_wstring(adapterDescription.AdapterLuid.LowPart);
            }
        }
        return {};
    }

    struct MediaSelection
    {
        fs::path path;
        std::string kind{ "video" };
        std::string id;
        bool sourceBacked{};
    };

    MediaSelection media_by_id(fs::path const& root, std::string const& groupId,
        std::string const& mediaId, std::string const& performanceMode)
    {
        if (!motion::valid_id(groupId) || !motion::valid_id(mediaId)) return {};
        auto directory = root / L"Wallpapers" / L"Groups" / motion::utf8_to_wide(groupId) / L"Videos" / motion::utf8_to_wide(mediaId);
        motion::MediaMetadata metadata;
        if (!motion::try_load_media(directory / L"metadata.json", metadata) ||
            (metadata.kind != "video" && metadata.kind != "image")) return {};
        if (metadata.id != mediaId || metadata.groupId != groupId || !motion::safe_file_name(metadata.fileName)) return {};
        auto fileName = fs::path(metadata.fileName);
        auto path = directory / fileName;
        std::error_code error;
        if (fs::is_regular_file(path, error) && !error) {
            return MediaSelection{ path, metadata.kind, mediaId, true };
        }
        if (metadata.kind != "video") return {};
        auto retained = motion::select_variant_file(
            motion::inspect_variant_cache(directory), performanceMode);
        if (retained.empty()) return {};
        auto retainedPath = directory / L"Variants" / retained;
        error.clear();
        return fs::is_regular_file(retainedPath, error) && !error
            ? MediaSelection{ std::move(retainedPath), "video", mediaId, false }
            : MediaSelection{};
    }

    struct ImportedOptimizationRequest
    {
        MediaSelection media;
        std::string mode;
    };

    std::vector<ImportedOptimizationRequest> imported_optimization_requests(fs::path const& root)
    {
        std::vector<ImportedOptimizationRequest> result;
        std::error_code error;
        auto groups = root / L"Wallpapers" / L"Groups";
        for (fs::directory_iterator groupEntries(groups, fs::directory_options::skip_permission_denied, error), groupEnd;
            !error && groupEntries != groupEnd; groupEntries.increment(error)) {
            std::error_code groupError;
            if (!groupEntries->is_directory(groupError) || groupError) continue;
            auto videos = groupEntries->path() / L"Videos";
            for (fs::directory_iterator mediaEntries(videos, fs::directory_options::skip_permission_denied, groupError), mediaEnd;
                !groupError && mediaEntries != mediaEnd; mediaEntries.increment(groupError)) {
                std::error_code mediaError;
                if (!mediaEntries->is_directory(mediaError) || mediaError) continue;
                auto mode = motion::read_variant_request(mediaEntries->path());
                if (mode.empty()) continue;
                motion::MediaMetadata metadata;
                if (!motion::try_load_media(mediaEntries->path() / L"metadata.json", metadata) ||
                    metadata.kind != "video" || !motion::safe_file_name(metadata.fileName)) continue;
                auto source = mediaEntries->path() / metadata.fileName;
                if (!fs::is_regular_file(source, mediaError) || mediaError) continue;
                result.push_back({ { std::move(source), "video", metadata.id, true }, std::move(mode) });
            }
        }
        return result;
    }

    struct OptimizationTarget
    {
        uint32_t width{};
        uint32_t height{};
        uint32_t refreshRateHz{};
    };

    OptimizationTarget optimization_target_size(motion::Settings const& settings)
    {
        auto displays = motion::enumerate_displays();
        if (settings.displayMode == "primary") {
            auto primary = std::find_if(displays.begin(), displays.end(), [](auto const& value) { return value.primary; });
            if (primary == displays.end()) return {};
            return { static_cast<uint32_t>(primary->bounds.right - primary->bounds.left),
                static_cast<uint32_t>(primary->bounds.bottom - primary->bounds.top), primary->refreshRateHz };
        }
        OptimizationTarget result;
        for (auto const& display : displays) {
            result.width = (std::max)(result.width,
                static_cast<uint32_t>(display.bounds.right - display.bounds.left));
            result.height = (std::max)(result.height,
                static_cast<uint32_t>(display.bounds.bottom - display.bounds.top));
            result.refreshRateHz = (std::max)(result.refreshRateHz, display.refreshRateHz);
        }
        return result;
    }

    std::vector<std::string> group_media_ids(fs::path const& root, std::string const& groupId,
        std::string const& performanceMode)
    {
        std::vector<std::string> result;
        if (!motion::valid_id(groupId)) return result;
        std::error_code error;
        auto directory = root / L"Wallpapers" / L"Groups" / motion::utf8_to_wide(groupId) / L"Videos";
        for (fs::directory_iterator entries(directory, error), end; !error && entries != end; entries.increment(error)) {
            std::error_code typeError;
            if (!entries->is_directory(typeError) || typeError) continue;
            auto id = entries->path().filename().string();
            if (motion::valid_id(id) && !media_by_id(root, groupId, id, performanceMode).path.empty()) {
                result.push_back(std::move(id));
            }
        }
        return result;
    }

    std::string random_media_id(fs::path const& root, std::string const& groupId,
        std::string const& previous, std::string const& performanceMode)
    {
        auto ids = group_media_ids(root, groupId, performanceMode);
        if (ids.empty()) return {};
        if (ids.size() > 1) ids.erase(std::remove(ids.begin(), ids.end(), previous), ids.end());
        static std::mt19937_64 generator{ std::random_device{}() };
        return ids[std::uniform_int_distribution<size_t>(0, ids.size() - 1)(generator)];
    }

    bool request_display_off() noexcept
    {
        DWORD_PTR result{};
        SetLastError(ERROR_SUCCESS);
        return SendMessageTimeoutW(
            HWND_BROADCAST,
            WM_SYSCOMMAND,
            SC_MONITORPOWER,
            2,
            SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            1000,
            &result) != 0;
    }

    class Renderer
    {
    public:
        enum class Target { Unknown, DesktopPlay, DesktopFreeze, ScreensaverPlay, Paused };

        explicit Renderer(fs::path executable) : executable_(std::move(executable)) {}
        ~Renderer() { Stop(); }

        bool Apply(Target target, MediaSelection const& media, std::string const& decodeMode,
            std::string const& displayMode, std::vector<std::wstring> const& monitorDevices = {})
        {
            auto now = std::chrono::steady_clock::now();
            std::wstring requestKey = media.path.wstring() + L"\n" + motion::utf8_to_wide(media.kind) + L"\n" +
                motion::utf8_to_wide(decodeMode) + L"\n" + motion::utf8_to_wide(displayMode);
            for (auto const& monitor : monitorDevices) requestKey += L"\n" + monitor;
            bool configurationChanged = requestKey != requestedKey_;
            if (configurationChanged && process_) Stop();
            if (configurationChanged) {
                requestedKey_ = std::move(requestKey);
                ResetBackOff();
            }
            Refresh();
            if (failed_.exchange(false)) FailAndBackOff();
            if (!process_) {
                if (now < nextLaunchAllowed_) return false;
                if (!Launch(media, decodeMode, displayMode, monitorDevices)) {
                    RecordFailure();
                    return false;
                }
            }
            bool awaitingAck = targetRevision_ && targetAcknowledgedRevision_.load() < targetRevision_;
            if (awaitingAck && now - targetFirstSentAt_ >= 6s) {
                FailAndBackOff();
                return false;
            }
            if (target != target_ || (awaitingAck && now - targetSentAt_ >= 2s)) {
                if (target != target_) targetFirstSentAt_ = now;
                target_ = target;
                targetRevision_ = Send(TargetName(target));
                targetSentAt_ = now;
            }
            if (TargetReady() && now - launchedAt_ >= 15s) ResetBackOff();
            return true;
        }

        void Pause()
        {
            Refresh();
            if (failed_.exchange(false)) { FailAndBackOff(); return; }
            auto now = std::chrono::steady_clock::now();
            bool awaitingAck = targetRevision_ && targetAcknowledgedRevision_.load() < targetRevision_;
            if (awaitingAck && now - targetFirstSentAt_ >= 6s) { FailAndBackOff(); return; }
            if (process_ && target_ != Target::Paused) {
                target_ = Target::Paused;
                targetRevision_ = Send("pause");
                targetSentAt_ = targetFirstSentAt_ = now;
            }
        }

        [[nodiscard]] bool TargetReady() const noexcept
        {
            return process_ && targetRevision_ &&
                targetAcknowledgedRevision_.load(std::memory_order_acquire) >= targetRevision_;
        }

        [[nodiscard]] motion::protocol::DecodeStatus DecodeState() const
        {
            std::scoped_lock lock(decodeStatusMutex_);
            return decodeStatus_;
        }

        void Stop()
        {
            Shutdown(true);
        }

    private:
        void Shutdown(bool resetBackOff)
        {
            if (process_) {
                Send("stop");
                if (WaitForSingleObject(process_.get(), 1200) == WAIT_TIMEOUT) TerminateProcess(process_.get(), 0);
                process_.reset();
            }
            job_.reset();
            ResetTransport();
            media_.clear();
            kind_.clear();
            decodeMode_.clear();
            monitorDevices_.clear();
            target_ = Target::Unknown;
            targetRevision_ = 0;
            failed_.store(false);
            if (resetBackOff) {
                ResetBackOff();
                requestedKey_.clear();
                std::scoped_lock lock(decodeStatusMutex_);
                decodeStatus_ = {};
            }
        }

        void ResetBackOff() noexcept
        {
            failureCount_ = 0;
            nextLaunchAllowed_ = std::chrono::steady_clock::time_point::min();
        }

        void RecordFailure() noexcept
        {
            failureCount_ = (std::min)(failureCount_ + 1u, 7u);
            auto delay = std::chrono::milliseconds(500u << (failureCount_ - 1));
            nextLaunchAllowed_ = std::chrono::steady_clock::now() +
                (std::min)(delay, std::chrono::duration_cast<std::chrono::milliseconds>(30s));
        }

        void FailAndBackOff()
        {
            Shutdown(false);
            RecordFailure();
        }

        static char const* TargetName(Target target)
        {
            switch (target) {
            case Target::DesktopPlay: return "desktop-play";
            case Target::DesktopFreeze: return "desktop-freeze";
            case Target::ScreensaverPlay: return "screensaver-play";
            default: return "pause";
            }
        }

        bool Launch(MediaSelection const& media, std::string const& decodeMode,
            std::string const& displayMode, std::vector<std::wstring> const& monitorDevices)
        {
            Shutdown(false);
            if (media.path.empty() || !fs::is_regular_file(executable_)) return false;

            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE inputReadRaw{}, inputWriteRaw{}, outputReadRaw{}, outputWriteRaw{};
            if (!CreatePipe(&inputReadRaw, &inputWriteRaw, &security, 0)) return false;
            motion::unique_handle inputRead(inputReadRaw);
            inputWrite_.reset(inputWriteRaw);
            SetHandleInformation(inputWrite_.get(), HANDLE_FLAG_INHERIT, 0);
            if (!CreatePipe(&outputReadRaw, &outputWriteRaw, &security, 0)) { inputWrite_.reset(); return false; }
            outputRead_.reset(outputReadRaw);
            motion::unique_handle outputWrite(outputWriteRaw);
            SetHandleInformation(outputRead_.get(), HANDLE_FLAG_INHERIT, 0);
            motion::unique_handle nullOutput(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

            wchar_t hidden[2]{};
            bool launchHidden = GetEnvironmentVariableW(L"MOTIONWALLPAPER_RENDERER_HIDDEN", hidden, ARRAYSIZE(hidden)) && hidden[0] == L'1';
            std::vector<std::wstring> arguments{
                executable_.wstring(), launchHidden ? L"-hidden" : L"-desktop", L"-video", media.path.wstring(),
                L"-kind", motion::utf8_to_wide(media.kind), L"-decode", motion::utf8_to_wide(decodeMode),
                L"-display", motion::utf8_to_wide(displayMode)
            };
            for (auto const& monitorDevice : monitorDevices) {
                arguments.push_back(L"-monitor");
                arguments.push_back(monitorDevice);
            }
            auto command = motion::build_command_line(arguments);
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            startup.hStdInput = inputRead.get();
            startup.hStdOutput = outputWrite.get();
            startup.hStdError = nullOutput.get();
            PROCESS_INFORMATION created{};
            if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                nullptr, executable_.parent_path().c_str(), &startup, &created)) {
                ResetTransport();
                return false;
            }
            motion::unique_handle createdThread(created.hThread);
            process_.reset(created.hProcess);
            job_.reset(CreateJobObjectW(nullptr, nullptr));
            if (job_) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                    !AssignProcessToJobObject(job_.get(), process_.get())) {
                    job_.reset();
                }
            }
            if (ResumeThread(createdThread.get()) == static_cast<DWORD>(-1)) {
                TerminateProcess(process_.get(), ERROR_PROCESS_ABORTED);
                Shutdown(false);
                return false;
            }
            media_ = media.path;
            kind_ = media.kind;
            decodeMode_ = decodeMode;
            displayMode_ = displayMode;
            monitorDevices_ = monitorDevices;
            target_ = Target::Unknown;
            targetAcknowledgedRevision_.store(0);
            failed_.store(false);
            launchedAt_ = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock(decodeStatusMutex_);
                decodeStatus_ = { "probing", "detecting" };
            }
            outputThread_ = std::thread([this] { ReadAcks(); });
            return true;
        }

        uint64_t Send(std::string const& command)
        {
            if (!inputWrite_) return 0;
            uint64_t revision = ++revision_;
            std::string value = command + " " + std::to_string(revision) + "\n";
            DWORD written{};
            if (!WriteFile(inputWrite_.get(), value.data(), static_cast<DWORD>(value.size()), &written, nullptr)) Refresh();
            return revision;
        }

        void ReadAcks()
        {
            std::string pending;
            char buffer[256];
            DWORD bytes{};
            while (outputRead_ && ReadFile(outputRead_.get(), buffer, sizeof(buffer), &bytes, nullptr) && bytes) {
                pending.append(buffer, bytes);
                for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                    auto value = pending.substr(0, end);
                    auto ack = motion::protocol::parse_ack(value);
                    if (ack.channel == motion::protocol::AckChannel::Target) {
                        uint64_t previous = targetAcknowledgedRevision_.load();
                        while (previous < ack.revision && !targetAcknowledgedRevision_.compare_exchange_weak(previous, ack.revision)) {}
                    } else if (auto status = motion::protocol::parse_decode_status(value); !status.path.empty()) {
                        std::scoped_lock lock(decodeStatusMutex_);
                        decodeStatus_ = std::move(status);
                    } else if (value.starts_with("error ")) {
                        failed_.store(true);
                    }
                }
            }
        }

        void Refresh()
        {
            if (!process_ || WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) return;
            Shutdown(false);
            RecordFailure();
        }

        void ResetTransport()
        {
            inputWrite_.reset();
            if (outputThread_.joinable()) outputThread_.join();
            outputRead_.reset();
        }

        fs::path executable_;
        motion::unique_handle process_;
        motion::unique_handle job_;
        motion::unique_handle inputWrite_;
        motion::unique_handle outputRead_;
        std::thread outputThread_;
        fs::path media_;
        std::string kind_;
        std::string decodeMode_;
        std::string displayMode_{ "primary" };
        std::vector<std::wstring> monitorDevices_;
        std::wstring requestedKey_;
        Target target_{ Target::Unknown };
        uint64_t revision_{};
        uint64_t targetRevision_{};
        std::atomic_uint64_t targetAcknowledgedRevision_{};
        std::atomic_bool failed_{};
        mutable std::mutex decodeStatusMutex_;
        motion::protocol::DecodeStatus decodeStatus_;
        std::chrono::steady_clock::time_point targetSentAt_{};
        std::chrono::steady_clock::time_point targetFirstSentAt_{};
        std::chrono::steady_clock::time_point launchedAt_{};
        std::chrono::steady_clock::time_point nextLaunchAllowed_{};
        unsigned failureCount_{};
    };

    struct DisplayMediaTarget
    {
        std::wstring deviceName;
        std::string groupId;
        std::string mediaId;
        MediaSelection media;
        uint32_t targetWidth{};
        uint32_t targetHeight{};
        uint32_t targetRefreshRate{};
    };

    std::vector<DisplayMediaTarget> display_media_targets(fs::path const& root, motion::Settings const& settings,
        std::string const& defaultGroupId, std::string const& defaultMediaId)
    {
        auto defaultMedia = media_by_id(root, defaultGroupId, defaultMediaId, settings.performanceMode);
        auto displays = motion::enumerate_displays();
        if (settings.displayMode == "primary") {
            if (defaultMedia.path.empty()) return {};
            auto primary = std::find_if(displays.begin(), displays.end(), [](auto const& display) { return display.primary; });
            uint32_t width = primary == displays.end() ? 0u : static_cast<uint32_t>(primary->bounds.right - primary->bounds.left);
            uint32_t height = primary == displays.end() ? 0u : static_cast<uint32_t>(primary->bounds.bottom - primary->bounds.top);
            uint32_t refreshRate = primary == displays.end() ? 60u : primary->refreshRateHz;
            return { { {}, defaultGroupId, defaultMediaId, std::move(defaultMedia), width, height, refreshRate } };
        }

        std::vector<DisplayMediaTarget> targets;
        for (auto const& display : displays) {
            auto width = static_cast<uint32_t>(display.bounds.right - display.bounds.left);
            auto height = static_cast<uint32_t>(display.bounds.bottom - display.bounds.top);
            auto groupId = defaultGroupId;
            auto mediaId = defaultMediaId;
            auto assignment = std::find_if(settings.displayAssignments.begin(), settings.displayAssignments.end(),
                [&](auto const& value) { return value.displayId == display.id; });
            if (assignment != settings.displayAssignments.end()) {
                auto assigned = media_by_id(
                    root, assignment->groupId, assignment->mediaId, settings.performanceMode);
                if (!assigned.path.empty()) {
                    groupId = assignment->groupId;
                    mediaId = assignment->mediaId;
                    targets.push_back({ display.deviceName,
                        std::move(groupId), std::move(mediaId), std::move(assigned), width, height, display.refreshRateHz });
                    continue;
                }
            }
            if (!defaultMedia.path.empty()) {
                targets.push_back({ display.deviceName,
                    std::move(groupId), std::move(mediaId), defaultMedia, width, height, display.refreshRateHz });
            }
        }
        return targets;
    }

    class RendererPool
    {
    public:
        explicit RendererPool(fs::path executable) : executable_(std::move(executable)) {}

        void Apply(Renderer::Target target, std::vector<DisplayMediaTarget> const& outputs,
            std::string const& decodeMode, bool primaryOnly)
        {
            std::vector<motion::agent::RendererRoute> routes;
            routes.reserve(outputs.size());
            for (auto const& output : outputs) {
                routes.push_back({ motion::agent::renderer_media_key(output.media.path, output.media.kind),
                    output.deviceName, primaryOnly ? std::wstring{} : display_adapter_key(output.deviceName) });
            }
            auto grouped = motion::agent::group_renderer_routes(routes, !primaryOnly);
            std::vector<std::wstring> desiredKeys;
            desiredKeys.reserve(grouped.size());
            bool allReady = !grouped.empty();
            for (auto const& route : grouped) {
                auto output = std::find_if(outputs.begin(), outputs.end(), [&](auto const& value) {
                    return motion::agent::renderer_media_key(value.media.path, value.media.kind) == route.mediaKey;
                });
                if (output == outputs.end()) continue;
                auto routeKey = RouteKey(route, decodeMode, primaryOnly);
                desiredKeys.push_back(routeKey);
                auto& renderer = renderers_[routeKey];
                if (!renderer) renderer = std::make_unique<Renderer>(executable_);
                bool applied = renderer->Apply(target, output->media, decodeMode,
                    primaryOnly ? "primary" : "monitor", route.monitorDevices);
                allReady = applied && renderer->TargetReady() && allReady;
            }
            desiredKeys_ = desiredKeys;
            if (allReady) {
                for (auto iterator = renderers_.begin(); iterator != renderers_.end();) {
                    if (std::find(desiredKeys.begin(), desiredKeys.end(), iterator->first) == desiredKeys.end()) {
                        iterator = renderers_.erase(iterator);
                    } else {
                        ++iterator;
                    }
                }
            }
        }

        void Pause()
        {
            for (auto& [_, renderer] : renderers_) renderer->Pause();
        }

        [[nodiscard]] bool TargetReady() const
        {
            return !desiredKeys_.empty() && std::all_of(desiredKeys_.begin(), desiredKeys_.end(), [&](auto const& key) {
                auto found = renderers_.find(key);
                return found != renderers_.end() && found->second->TargetReady();
            });
        }

        [[nodiscard]] bool HasActiveRoute() const noexcept { return !desiredKeys_.empty(); }

        [[nodiscard]] motion::protocol::DecodeStatus DecodeState() const
        {
            motion::protocol::DecodeStatus aggregate;
            int aggregateRank = -1;
            auto rank = [](std::string const& path) {
                if (path == "unavailable") return 6;
                if (path == "software-fallback") return 5;
                if (path == "software") return 4;
                if (path == "hardware") return 3;
                if (path == "not-applicable") return 2;
                if (path == "probing") return 1;
                return 0;
            };
            for (auto const& key : desiredKeys_) {
                auto found = renderers_.find(key);
                if (found == renderers_.end()) continue;
                auto status = found->second->DecodeState();
                int statusRank = rank(status.path);
                if (statusRank > aggregateRank) {
                    aggregateRank = statusRank;
                    aggregate = std::move(status);
                }
            }
            return aggregate;
        }

        void TopologyChanged() noexcept { ++topologyGeneration_; }
        void Stop() { desiredKeys_.clear(); renderers_.clear(); }

    private:
        std::wstring RouteKey(motion::agent::SharedRendererRoute const& route,
            std::string const& decodeMode, bool primaryOnly) const
        {
            std::wstring key = route.mediaKey + L"\n" + motion::utf8_to_wide(decodeMode) +
                (primaryOnly ? L"\nprimary\n" : L"\nmonitor\n") + route.adapterKey + L"\n" +
                std::to_wstring(topologyGeneration_);
            for (auto const& monitor : route.monitorDevices) key += L"\n" + monitor;
            return key;
        }

        fs::path executable_;
        std::map<std::wstring, std::unique_ptr<Renderer>> renderers_;
        std::vector<std::wstring> desiredKeys_;
        uint64_t topologyGeneration_{};
    };

    struct InputState
    {
        DWORD tick{};
        std::chrono::milliseconds idle{};
    };

    InputState input_state()
    {
        LASTINPUTINFO input{ sizeof(input) };
        if (!GetLastInputInfo(&input)) return {};
        auto currentTick = static_cast<DWORD>(GetTickCount64());
        return { input.dwTime, std::chrono::milliseconds(static_cast<DWORD>(currentTick - input.dwTime)) };
    }

    struct IdleInhibition
    {
        bool display{};
        bool system{};
    };

    class IdleInhibitor
    {
    public:
        IdleInhibition State(std::chrono::steady_clock::time_point now)
        {
            if (now < nextSample_) return state_;
            nextSample_ = now + 500ms;
            EXECUTION_STATE state{};
            if (CallNtPowerInformation(SystemExecutionState, nullptr, 0, &state, sizeof(state)) == ERROR_SUCCESS) {
                state_.display = (state & ES_DISPLAY_REQUIRED) != 0;
                state_.system = (state & ES_SYSTEM_REQUIRED) != 0;
            } else {
                state_ = {};
            }
            return state_;
        }

    private:
        IdleInhibition state_{};
        std::chrono::steady_clock::time_point nextSample_{};
    };

    std::optional<bool> query_session_locked()
    {
        LPWSTR buffer{};
        DWORD bytes{};
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSConnectState, &buffer, &bytes)) return std::nullopt;
        auto release = [](wchar_t* value) noexcept { if (value) WTSFreeMemory(value); };
        std::unique_ptr<wchar_t, decltype(release)> memory(buffer, release);
        if (!buffer || bytes < sizeof(WTS_CONNECTSTATE_CLASS)) return std::nullopt;
        auto state = *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(buffer);
        return state != WTSActive;
    }

    class RuntimeEvents
    {
    public:
        explicit RuntimeEvents(fs::path settingsExecutable) : settingsExecutable_(std::move(settingsExecutable))
        {
            appExitEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, motion::app_exit_event_name));
            if (auto locked = query_session_locked()) locked_ = *locked;
            taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
            WNDCLASSEXW definition{ sizeof(definition) };
            definition.lpfnWndProc = WindowProc;
            definition.hInstance = GetModuleHandleW(nullptr);
            definition.lpszClassName = L"MotionWallpaper.Agent.Events";
            if (!RegisterClassExW(&definition) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
            window_ = CreateWindowExW(0, definition.lpszClassName, L"", WS_OVERLAPPED,
                0, 0, 0, 0, nullptr, nullptr, definition.hInstance, this);
            if (!window_) return;
            sessionNotificationRegistered_ =
                WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) != FALSE;
            displayNotification_ = RegisterPowerSettingNotification(window_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
            eventWindow_.store(window_, std::memory_order_release);
            foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            minimizeHook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr,
                ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            locationHook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            AddTrayIcon();
        }

        ~RuntimeEvents()
        {
            if (foregroundHook_) UnhookWinEvent(foregroundHook_);
            if (minimizeHook_) UnhookWinEvent(minimizeHook_);
            if (locationHook_) UnhookWinEvent(locationHook_);
            HWND expected = window_;
            eventWindow_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
            NOTIFYICONDATAW icon{ sizeof(icon) };
            icon.hWnd = window_;
            icon.uID = trayIconId;
            if (window_) Shell_NotifyIconW(NIM_DELETE, &icon);
            if (displayNotification_) UnregisterPowerSettingNotification(displayNotification_);
            if (window_) {
                if (sessionNotificationRegistered_) WTSUnRegisterSessionNotification(window_);
                DestroyWindow(window_);
            }
        }

        explicit operator bool() const { return window_ != nullptr && static_cast<bool>(appExitEvent_); }
        bool Locked() const { return locked_; }
        bool DisplayOn() const { return displayOn_; }
        uint64_t TopologyRevision() const { return topologyRevision_; }
        bool ExitRequested() const { return exitRequested_; }

        void PollSessionState(std::chrono::steady_clock::time_point now)
        {
            if (now < nextSessionPoll_) return;
            nextSessionPoll_ = now + 1s;
            if (auto locked = query_session_locked()) locked_ = *locked;
        }

        bool Wait(HANDLE settingsEvent, DWORD milliseconds)
        {
            HANDLE handles[]{ settingsEvent, appExitEvent_.get() };
            DWORD result = MsgWaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, milliseconds, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0) return true;
            if (result == WAIT_OBJECT_0 + 1) {
                exitRequested_ = true;
                return false;
            }
            if (result == WAIT_OBJECT_0 + ARRAYSIZE(handles)) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            return false;
        }

    private:
        static constexpr UINT wmTrayIcon = WM_APP + 100;
        static constexpr UINT wmForegroundChanged = WM_APP + 101;
        static constexpr UINT trayIconId = 1;
        static inline std::atomic<HWND> eventWindow_{};
        static inline std::atomic_bool foregroundWakePending_{};

        static void CALLBACK ForegroundEvent(HWINEVENTHOOK, DWORD event, HWND, LONG object, LONG child, DWORD, DWORD)
        {
            if (event == EVENT_OBJECT_LOCATIONCHANGE && (object != OBJID_WINDOW || child != CHILDID_SELF)) return;
            if (!foregroundWakePending_.exchange(true, std::memory_order_acq_rel)) {
                if (HWND window = eventWindow_.load(std::memory_order_acquire); !window || !PostMessageW(window, wmForegroundChanged, 0, 0)) {
                    foregroundWakePending_.store(false, std::memory_order_release);
                }
            }
        }

        void AddTrayIcon()
        {
            if (!window_) return;
            NOTIFYICONDATAW icon{ sizeof(icon) };
            icon.hWnd = window_;
            icon.uID = trayIconId;
            icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            icon.uCallbackMessage = wmTrayIcon;
            icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(icon.szTip, L"MotionWallpaper");
            Shell_NotifyIconW(NIM_ADD, &icon);
            icon.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &icon);
        }

        void OpenSettings() const
        {
            if (fs::is_regular_file(settingsExecutable_)) {
                ShellExecuteW(nullptr, L"open", settingsExecutable_.c_str(), nullptr, settingsExecutable_.parent_path().c_str(), SW_SHOWNORMAL);
            }
        }

        void ShowTrayMenu()
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            HMENU menu = CreatePopupMenu();
            if (!menu) return;
            AppendMenuW(menu, MF_STRING, 1, L"打开 MotionWallpaper");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, L"退出");
            SetForegroundWindow(window_);
            UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                cursor.x, cursor.y, 0, window_, nullptr);
            DestroyMenu(menu);
            if (command == 1) OpenSettings();
            else if (command == 2) {
                if (appExitEvent_) SetEvent(appExitEvent_.get());
                exitRequested_ = true;
            }
        }

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            RuntimeEvents* self = reinterpret_cast<RuntimeEvents*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE) {
                auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                self = static_cast<RuntimeEvents*>(create->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (!self) return DefWindowProcW(window, message, wParam, lParam);
            if (self->taskbarCreated_ && message == self->taskbarCreated_) {
                ++self->topologyRevision_;
                self->AddTrayIcon();
                return 0;
            }
            if (message == wmTrayIcon) {
                auto event = LOWORD(lParam);
                if (event == WM_LBUTTONDBLCLK) self->OpenSettings();
                else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) self->ShowTrayMenu();
                return 0;
            }
            if (message == wmForegroundChanged) {
                foregroundWakePending_.store(false, std::memory_order_release);
                return 0;
            }
            switch (message) {
            case WM_WTSSESSION_CHANGE:
                switch (wParam) {
                case WTS_SESSION_LOCK:
                case WTS_CONSOLE_DISCONNECT:
                case WTS_REMOTE_DISCONNECT:
                case WTS_SESSION_LOGOFF:
                    self->locked_ = true;
                    break;
                case WTS_SESSION_UNLOCK:
                case WTS_CONSOLE_CONNECT:
                case WTS_REMOTE_CONNECT:
                case WTS_SESSION_LOGON:
                case WTS_SESSION_DESKTOP_READY:
                    self->locked_ = false;
                    break;
                }
                return 0;
            case WM_DISPLAYCHANGE:
                ++self->topologyRevision_;
                return 0;
            case WM_POWERBROADCAST:
                if (wParam == PBT_POWERSETTINGCHANGE) {
                    auto setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
                    if (setting && IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) && setting->DataLength >= sizeof(DWORD)) {
                        self->displayOn_ = *reinterpret_cast<DWORD*>(setting->Data) != 0;
                    }
                }
                return TRUE;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }

        HWND window_{};
        HPOWERNOTIFY displayNotification_{};
        HWINEVENTHOOK foregroundHook_{};
        HWINEVENTHOOK minimizeHook_{};
        HWINEVENTHOOK locationHook_{};
        UINT taskbarCreated_{};
        fs::path settingsExecutable_;
        motion::unique_handle appExitEvent_;
        bool locked_{};
        bool sessionNotificationRegistered_{};
        bool displayOn_{ true };
        bool exitRequested_{};
        uint64_t topologyRevision_{};
        std::chrono::steady_clock::time_point nextSessionPoll_{};
    };

    bool shell_window(HWND window)
    {
        wchar_t name[128]{};
        GetClassNameW(window, name, ARRAYSIZE(name));
        return !_wcsicmp(name, L"Progman") || !_wcsicmp(name, L"WorkerW") || !_wcsicmp(name, L"SHELLDLL_DefView") || !_wcsicmp(name, L"Shell_TrayWnd");
    }

    motion::agent::WindowBounds window_bounds(RECT const& value)
    {
        return { value.left, value.top, value.right, value.bottom };
    }

    bool visible_application_window(HWND window)
    {
        if (!window || !IsWindowVisible(window) || IsIconic(window) || shell_window(window)) return false;
        DWORD cloaked{};
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
        if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED) {
            BYTE alpha{};
            DWORD flags{};
            if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) return false;
        }
        return true;
    }

    bool window_covers_display(HWND window)
    {
        if (!visible_application_window(window)) return false;
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        MONITORINFO info{ sizeof(info) };
        if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) && !GetWindowRect(window, &bounds)) return false;
        auto windowBounds = window_bounds(bounds);
        return motion::agent::covers_display(windowBounds, window_bounds(info.rcMonitor)) ||
            motion::agent::covers_display(windowBounds, window_bounds(info.rcWork));
    }

    bool desktop_covered()
    {
        bool covered{};
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto result = reinterpret_cast<bool*>(parameter);
            if (!window_covers_display(window)) return TRUE;
            *result = true;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&covered));
        return covered;
    }

    void enable_eco_qos()
    {
        struct PowerThrottlingState { ULONG version; ULONG controlMask; ULONG stateMask; };
        constexpr ULONG processPowerThrottling = 4;
        constexpr ULONG executionSpeed = 0x1;
        PowerThrottlingState state{ 1, executionSpeed, executionSpeed };
        SetProcessInformation(GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(processPowerThrottling), &state, sizeof(state));
    }

    void append_agent_log(fs::path const& root, std::wstring_view message) noexcept
    {
        motion::append_utf8_log(root / L"Config" / L"agent.log", message);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    motion::enable_per_monitor_dpi_awareness();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    motion::unique_handle mutex(CreateMutexW(nullptr, TRUE, L"Local\\MotionWallpaper.Agent"));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    motion::unique_handle settingsEvent(CreateEventW(nullptr, FALSE, FALSE, motion::settings_event_name));
    if (!settingsEvent) return 1;
    enable_eco_qos();
    auto root = motion::executable_directory();

    try {
        auto configPath = root / L"Config" / L"settings.json";
        auto runtimePath = root / L"Config" / L"runtime.json";
        motion::Settings settings;
        RendererPool renderers(root / L"motionwallpaper-renderer.exe");
        motion::agent::VideoOptimizer videoOptimizer(root);
        RuntimeEvents runtimeEvents(root / L"MotionWallpaper.exe");
        if (!runtimeEvents) return 1;
        std::string previousRandomGroup, previousSelectedGroup, previousSelectedMedia, previousPerformanceMode, randomId;
        int previousRandomInterval = -1;
        bool wasLocked = false, autoLockTriggered = false, displayOffAfterLockTriggered = false;
        bool reload = true, selectionInitialized = false;
        motion::IdleTimer screensaverIdleTimer;
        motion::IdleTimer autoLockIdleTimer;
        bool screensaverWasActive{};
        IdleInhibitor idleInhibitor;
        auto nextFallbackReload = std::chrono::steady_clock::now();
        auto nextOptimizationRequestScan = std::chrono::steady_clock::time_point::min();
        std::vector<ImportedOptimizationRequest> importedRequests;
        auto nextRandomChange = std::chrono::steady_clock::time_point::max();
        auto nextAutoLockAttempt = std::chrono::steady_clock::time_point::min();
        auto lockObservedAt = std::chrono::steady_clock::time_point::max();
        auto nextDisplayOffAfterLockAttempt = std::chrono::steady_clock::time_point::min();
        std::optional<std::chrono::steady_clock::time_point> missingMediaSince;
        uint64_t topologyRevision = runtimeEvents.TopologyRevision();
        std::string publishedGroupId, publishedMediaId, publishedDecodePath, publishedDecodeReason;
        bool configFailureReported{};
        bool runtimeFailureReported{};
        auto publishRuntime = [&](std::string groupId, std::string mediaId,
            std::string decodePath = {}, std::string decodeReason = {}) {
            if (groupId == publishedGroupId && mediaId == publishedMediaId &&
                decodePath == publishedDecodePath && decodeReason == publishedDecodeReason) return;
            try {
                motion::RuntimeState runtime;
                runtime.activeGroupId = groupId;
                runtime.activeMediaId = mediaId;
                runtime.decodePath = decodePath;
                runtime.decodeReason = decodeReason;
                runtime.updatedAt = motion::timestamp_utc();
                motion::save_runtime(runtimePath, runtime);
                publishedGroupId = std::move(groupId);
                publishedMediaId = std::move(mediaId);
                publishedDecodePath = std::move(decodePath);
                publishedDecodeReason = std::move(decodeReason);
                runtimeFailureReported = false;
            } catch (...) {
                if (!runtimeFailureReported) append_agent_log(root, L"无法写入运行时壁纸状态。");
                runtimeFailureReported = true;
            }
        };

        for (;;) {
            if (runtimeEvents.ExitRequested()) { renderers.Stop(); return 0; }
            auto now = std::chrono::steady_clock::now();
            runtimeEvents.PollSessionState(now);
            if (reload) nextOptimizationRequestScan = std::chrono::steady_clock::time_point::min();
            if (reload || now >= nextFallbackReload) {
                if (motion::try_load_settings(configPath, settings)) {
                    configFailureReported = false;
                } else {
                    std::error_code existsError;
                    bool exists = fs::is_regular_file(configPath, existsError);
                    if (!configFailureReported && exists && !existsError) {
                        append_agent_log(root, L"设置文件损坏或暂时不可读，继续使用上一份有效配置。");
                        configFailureReported = true;
                    }
                }
                reload = false;
                nextFallbackReload = now + 2s;
            }
            if (now >= nextOptimizationRequestScan) {
                importedRequests = imported_optimization_requests(root);
                nextOptimizationRequestScan = now + (importedRequests.empty() ? 10s : 1s);
            }
            auto prepareImported = [&] {
                if (importedRequests.empty()) return;
                auto target = optimization_target_size(settings);
                for (auto const& request : importedRequests) {
                    videoOptimizer.Prepare(request.media.path, request.mode,
                        target.width, target.height, target.refreshRateHz);
                }
            };

            bool locked = runtimeEvents.Locked();
            auto input = input_state();
            auto uptime = std::chrono::milliseconds(GetTickCount64());
            auto inhibition = idleInhibitor.State(now);
            auto screensaverIdle = screensaverIdleTimer.Update(uptime, input.tick, input.idle,
                motion::agent::screensaver_idle_is_inhibited(inhibition.display, screensaverWasActive));
            DWORD waitMilliseconds = 500;
            if (runtimeEvents.TopologyRevision() != topologyRevision) {
                topologyRevision = runtimeEvents.TopologyRevision();
                renderers.TopologyChanged();
            }
            auto sessionAction = motion::agent::reduce_runtime_action(settings,
                { runtimeEvents.DisplayOn(), locked, false, false, 0 });
            if (sessionAction == motion::agent::RuntimeAction::DisplayOff) {
                screensaverWasActive = false;
                // The display-off contract wins over background preparation:
                // stop hardware work now and resume the durable request later.
                videoOptimizer.SetGenerationAllowed(false);
                renderers.Stop();
                waitMilliseconds = 1000;
            } else if (sessionAction == motion::agent::RuntimeAction::Locked) {
                screensaverWasActive = false;
                videoOptimizer.SetGenerationAllowed(false);
                renderers.Stop();
                if (!wasLocked) {
                    wasLocked = true;
                    lockObservedAt = now;
                    displayOffAfterLockTriggered = false;
                    nextDisplayOffAfterLockAttempt = std::chrono::steady_clock::time_point::min();
                }
                if (motion::agent::display_off_after_lock_is_due(
                    settings.displayOffAfterLockEnabled,
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - lockObservedAt),
                    settings.displayOffAfterLockDelaySeconds,
                    displayOffAfterLockTriggered,
                    now >= nextDisplayOffAfterLockAttempt)) {
                    displayOffAfterLockTriggered = request_display_off();
                    if (!displayOffAfterLockTriggered) {
                        nextDisplayOffAfterLockAttempt = now + 5s;
                        append_agent_log(root, L"锁屏后熄屏请求失败，5 秒后重试，错误码 " +
                            std::to_wstring(GetLastError()) + L"。");
                    }
                }
                waitMilliseconds = 1000;
            } else {
                bool randomGroupChanged = settings.randomGroupId != previousRandomGroup;
                if (randomGroupChanged) previousRandomGroup = settings.randomGroupId;
                bool selectionChanged = selectionInitialized &&
                    (settings.selectedGroupId != previousSelectedGroup || settings.selectedMediaId != previousSelectedMedia);
                previousSelectedGroup = settings.selectedGroupId;
                previousSelectedMedia = settings.selectedMediaId;
                selectionInitialized = true;
                bool performanceModeChanged = settings.performanceMode != previousPerformanceMode;
                previousPerformanceMode = settings.performanceMode;
                if (selectionChanged || performanceModeChanged) videoOptimizer.InvalidateChoices();
                bool randomIntervalChanged = settings.randomIntervalMinutes != previousRandomInterval;
                if (randomIntervalChanged) {
                    previousRandomInterval = settings.randomIntervalMinutes;
                    nextRandomChange = settings.randomIntervalMinutes > 0
                        ? now + std::chrono::minutes(settings.randomIntervalMinutes)
                        : std::chrono::steady_clock::time_point::max();
                }
                bool timedChange = settings.randomIntervalMinutes > 0 && now >= nextRandomChange &&
                    screensaverIdle < std::chrono::seconds(settings.idleTimeoutSeconds);
                bool randomActive = motion::valid_id(settings.randomGroupId) &&
                    settings.randomGroupId == settings.selectedGroupId;
                bool currentRandomIsValid = randomActive && !randomId.empty() &&
                    !media_by_id(root, settings.randomGroupId, randomId, settings.performanceMode).path.empty();
                auto randomAction = motion::agent::random_selection_action(
                    randomActive, randomGroupChanged, selectionChanged, wasLocked, timedChange, currentRandomIsValid);
                if (randomAction == motion::agent::RandomSelectionAction::ChooseRandom) {
                    randomId = random_media_id(
                        root, settings.randomGroupId, randomId, settings.performanceMode);
                    nextRandomChange = settings.randomIntervalMinutes > 0
                        ? now + std::chrono::minutes(settings.randomIntervalMinutes)
                        : std::chrono::steady_clock::time_point::max();
                } else if (randomAction == motion::agent::RandomSelectionAction::UseSelected) {
                    randomId = randomActive ? settings.selectedMediaId : std::string{};
                    nextRandomChange = settings.randomIntervalMinutes > 0
                        ? now + std::chrono::minutes(settings.randomIntervalMinutes)
                        : std::chrono::steady_clock::time_point::max();
                }
                if (wasLocked) {
                    wasLocked = false;
                    autoLockTriggered = false;
                    displayOffAfterLockTriggered = false;
                    lockObservedAt = std::chrono::steady_clock::time_point::max();
                }

                auto groupId = randomId.empty() ? settings.selectedGroupId : settings.randomGroupId;
                auto mediaId = randomId.empty() ? settings.selectedMediaId : randomId;
                auto outputs = display_media_targets(root, settings, groupId, mediaId);
                bool selectedMediaTemporarilyMissing = outputs.empty() &&
                    motion::valid_id(groupId) && motion::valid_id(mediaId) && renderers.HasActiveRoute();
                if (selectedMediaTemporarilyMissing && !missingMediaSince) missingMediaSince = now;
                bool holdExistingRenderer = selectedMediaTemporarilyMissing && missingMediaSince &&
                    now - *missingMediaSince < 2s;
                if (!outputs.empty() || !selectedMediaTemporarilyMissing) missingMediaSince.reset();
                auto state = motion::agent::reduce_runtime_action(settings,
                    { true, false, desktop_covered(), !outputs.empty(), screensaverIdle.count() / 1000 });
                bool ownScreensaverActive = screensaverWasActive ||
                    state == motion::agent::RuntimeAction::ScreensaverPlay;
                auto autoLockIdle = autoLockIdleTimer.Update(uptime, input.tick, input.idle,
                    motion::agent::automatic_lock_idle_is_inhibited(
                        inhibition.display, inhibition.system, ownScreensaverActive));
                screensaverWasActive = state == motion::agent::RuntimeAction::ScreensaverPlay;
                // Explicit/import requests are durable and run at background
                // priority. Power saver is also allowed to prepare its selected
                // display-sized stream while the source remains visible. The
                // selected tier is never changed implicitly by power state.
                bool powerSaverNeedsPriority = settings.performanceMode == "power-saver";
                videoOptimizer.SetGenerationAllowed(
                    powerSaverNeedsPriority || !importedRequests.empty() ||
                    state != motion::agent::RuntimeAction::DesktopPlay &&
                    state != motion::agent::RuntimeAction::ScreensaverPlay);
                prepareImported();
                std::map<std::wstring, OptimizationTarget> requiredVideoTargets;
                for (auto const& output : outputs) {
                    if (output.media.kind != "video" || !output.media.sourceBacked) continue;
                    auto& required = requiredVideoTargets[output.media.path.wstring()];
                    required.width = (std::max)(required.width, output.targetWidth);
                    required.height = (std::max)(required.height, output.targetHeight);
                    required.refreshRateHz = (std::max)(required.refreshRateHz, output.targetRefreshRate);
                }
                for (auto& output : outputs) {
                    if (output.media.kind == "video" && output.media.sourceBacked) {
                        auto const required = requiredVideoTargets[output.media.path.wstring()];
                        output.media.path = videoOptimizer.Resolve(output.media.path, settings.performanceMode,
                            required.width, required.height, required.refreshRateHz);
                    }
                }

                if (settings.autoLockEnabled &&
                    autoLockIdle >= std::chrono::seconds(settings.autoLockTimeoutSeconds)) {
                    if (!autoLockTriggered && now >= nextAutoLockAttempt) {
                        // Stop decoding before requesting the secure desktop.
                        // Display power-off is independently scheduled only
                        // after WTS confirms that Windows is locked.
                        renderers.Stop();
                        if (LockWorkStation()) {
                            autoLockTriggered = true;
                            screensaverWasActive = false;
                        } else {
                            nextAutoLockAttempt = now + 5s;
                            append_agent_log(root, L"自动锁屏请求失败，5 秒后重试，错误码 " +
                                std::to_wstring(GetLastError()) + L"。");
                        }
                    }
                } else {
                    autoLockTriggered = false;
                    nextAutoLockAttempt = std::chrono::steady_clock::time_point::min();
                }

                bool targetReady{};
                if (autoLockTriggered) {
                    waitMilliseconds = 1000;
                } else if (holdExistingRenderer) {
                    // Import/move publishes metadata and files in separate atomic
                    // steps. Preserve the last fully presented frame during that
                    // short transaction instead of exposing the Windows wallpaper.
                    waitMilliseconds = 50;
                } else {
                    switch (state) {
                    case motion::agent::RuntimeAction::ScreensaverPlay:
                        renderers.Apply(Renderer::Target::ScreensaverPlay, outputs, settings.decodeMode,
                            settings.displayMode == "primary");
                        targetReady = renderers.TargetReady();
                        waitMilliseconds = 50;
                        break;
                    case motion::agent::RuntimeAction::DesktopPlay:
                        renderers.Apply(Renderer::Target::DesktopPlay, outputs, settings.decodeMode,
                            settings.displayMode == "primary");
                        targetReady = renderers.TargetReady();
                        break;
                    case motion::agent::RuntimeAction::DesktopFrozen:
                        renderers.Apply(Renderer::Target::DesktopFreeze, outputs, settings.decodeMode,
                            settings.displayMode == "primary");
                        targetReady = renderers.TargetReady();
                        break;
                    case motion::agent::RuntimeAction::DesktopPaused:
                        renderers.Pause();
                        targetReady = renderers.TargetReady();
                        break;
                    default:
                        renderers.Stop();
                        publishRuntime({}, {});
                        break;
                    }
                }
                if (targetReady) {
                    auto decode = renderers.DecodeState();
                    if (!groupId.empty() && !mediaId.empty()) publishRuntime(groupId, mediaId, decode.path, decode.reason);
                    else if (!outputs.empty()) publishRuntime(outputs.front().groupId, outputs.front().mediaId, decode.path, decode.reason);
                }
            }
            auto decode = renderers.DecodeState();
            publishRuntime(publishedGroupId, publishedMediaId, decode.path, decode.reason);
            reload = runtimeEvents.Wait(settingsEvent.get(), waitMilliseconds);
        }
    } catch (std::exception const& error) {
        try { append_agent_log(root, L"Agent 遇到致命错误: " + motion::utf8_to_wide(error.what())); } catch (...) {}
        return 1;
    } catch (...) {
        append_agent_log(root, L"Agent 遇到未知致命错误。");
        return 1;
    }
}
