#include "VideoTranscoder.h"

#include "../MotionWallpaper.Common/Common.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <set>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t vendorNvidia = 0x10de;
    constexpr uint32_t vendorIntel = 0x8086;
    constexpr uint32_t vendorAmd = 0x1002;
    constexpr uint64_t softwarePixelRateLimit =
        static_cast<uint64_t>(2560) * 1440 * 60;

    std::vector<motion::agent::VideoTranscodeAdapter> installed_adapters(bool& succeeded)
    {
        std::vector<motion::agent::VideoTranscodeAdapter> result;
        ComPtr<IDXGIFactory1> factory;
        succeeded = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        if (!succeeded) return result;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            auto status = factory->EnumAdapters1(index, &adapter);
            if (status == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(status)) {
                succeeded = false;
                result.clear();
                return result;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            result.push_back({ description.VendorId,
                static_cast<uint64_t>(description.DedicatedVideoMemory) });
        }
        return result;
    }

    uint32_t target_bitrate_kbps(uint32_t width, uint32_t height, uint32_t targetFps)
    {
        // Bound VBR so a performance copy cannot grow without limit merely
        // because one vendor's constant-quality implementation is aggressive.
        auto pixelsPerSecond = static_cast<uint64_t>(width) * height * targetFps;
        auto estimate = static_cast<uint32_t>((pixelsPerSecond * 65 + 999'999) / 1'000'000);
        return std::clamp(estimate, 4'000u, 60'000u);
    }

    void append_system_memory_input(std::vector<std::wstring>& arguments,
        fs::path const& source, uint32_t width, uint32_t height, std::wstring const& format)
    {
        arguments.insert(arguments.end(), {
            L"-i", source.wstring(), L"-map", L"0:v:0", L"-map_metadata", L"0", L"-an", L"-vf",
            L"scale=" + std::to_wstring(width) + L":" + std::to_wstring(height) +
                L":flags=lanczos,format=" + format
        });
    }

    void append_bounded_rate(std::vector<std::wstring>& arguments, uint32_t bitrateKbps)
    {
        auto maximum = bitrateKbps + bitrateKbps / 2;
        auto buffer = bitrateKbps * 2;
        arguments.insert(arguments.end(), {
            L"-b:v", std::to_wstring(bitrateKbps) + L"k",
            L"-maxrate", std::to_wstring(maximum) + L"k",
            L"-bufsize", std::to_wstring(buffer) + L"k"
        });
    }

    std::vector<std::wstring> transcode_arguments(
        fs::path const& ffmpeg,
        fs::path const& source,
        fs::path const& destination,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        motion::agent::VideoTranscodeBackend backend)
    {
        using motion::agent::VideoTranscodeBackend;
        std::vector<std::wstring> arguments{
            ffmpeg.wstring(), L"-nostdin", L"-hide_banner", L"-loglevel", L"error", L"-y"
        };
        auto bitrate = target_bitrate_kbps(width, height, targetFps);
        switch (backend) {
        case VideoTranscodeBackend::nvidiaCudaNvenc:
            arguments.insert(arguments.end(), {
                L"-hwaccel", L"cuda", L"-hwaccel_output_format", L"cuda", L"-i", source.wstring(),
                L"-map", L"0:v:0", L"-map_metadata", L"0", L"-an", L"-vf",
                L"scale_cuda=" + std::to_wstring(width) + L":" + std::to_wstring(height) + L":format=p010le",
                L"-c:v", L"hevc_nvenc", L"-preset", L"p5", L"-tune", L"hq",
                L"-profile:v", L"main10", L"-rc", L"vbr", L"-cq", L"21",
                L"-spatial-aq", L"1", L"-temporal-aq", L"1"
            });
            append_bounded_rate(arguments, bitrate);
            break;
        case VideoTranscodeBackend::nvidiaNvenc:
            append_system_memory_input(arguments, source, width, height, L"p010le");
            arguments.insert(arguments.end(), {
                L"-c:v", L"hevc_nvenc", L"-preset", L"p5", L"-tune", L"hq",
                L"-profile:v", L"main10", L"-rc", L"vbr", L"-cq", L"21",
                L"-spatial-aq", L"1", L"-temporal-aq", L"1"
            });
            append_bounded_rate(arguments, bitrate);
            break;
        case VideoTranscodeBackend::intelQsv:
            append_system_memory_input(arguments, source, width, height, L"p010le");
            arguments.insert(arguments.end(), {
                L"-c:v", L"hevc_qsv", L"-preset", L"slow", L"-profile:v", L"main10",
                L"-scenario", L"archive", L"-global_quality", L"21"
            });
            append_bounded_rate(arguments, bitrate);
            break;
        case VideoTranscodeBackend::amdAmf:
            append_system_memory_input(arguments, source, width, height, L"p010le");
            arguments.insert(arguments.end(), {
                L"-c:v", L"hevc_amf", L"-usage", L"high_quality", L"-quality", L"quality",
                L"-profile:v", L"main10", L"-bitdepth", L"10", L"-rc", L"qvbr",
                L"-qvbr_quality_level", L"21", L"-vbaq", L"1"
            });
            append_bounded_rate(arguments, bitrate);
            break;
        case VideoTranscodeBackend::softwareKvazaar:
            append_system_memory_input(arguments, source, width, height, L"yuv420p");
            arguments.insert(arguments.end(), {
                L"-c:v", L"libkvazaar", L"-kvazaar-params", L"threads=4"
            });
            append_bounded_rate(arguments, bitrate);
            break;
        }
        arguments.insert(arguments.end(), {
            L"-r", std::to_wstring(targetFps), L"-fps_mode", L"cfr", L"-tag:v", L"hvc1",
            L"-movflags", L"+faststart", destination.wstring()
        });
        return arguments;
    }

    motion::agent::VideoTranscodeResult run_ffmpeg(
        fs::path const& ffmpeg,
        std::vector<std::wstring> const& arguments,
        std::function<motion::agent::VideoTranscodeControl()> const& control,
        std::wstring& error)
    {
        using namespace motion::agent;
        auto command = motion::build_command_line(arguments);
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr, ffmpeg.parent_path().c_str(), &startup, &process)) {
            error = L"无法启动 FFmpeg 优化后端";
            return VideoTranscodeResult::failed;
        }
        motion::unique_handle processHandle{ process.hProcess };
        motion::unique_handle threadHandle{ process.hThread };
        motion::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job.get(), processHandle.get())) {
                job.reset();
            }
        }

        PROCESS_POWER_THROTTLING_STATE powerThrottling{};
        powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        powerThrottling.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(processHandle.get(), ProcessPowerThrottling,
            &powerThrottling, sizeof(powerThrottling));
        MEMORY_PRIORITY_INFORMATION memoryPriority{ MEMORY_PRIORITY_LOW };
        SetProcessInformation(processHandle.get(), ProcessMemoryPriority,
            &memoryPriority, sizeof(memoryPriority));
        if (ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1)) {
            TerminateProcess(processHandle.get(), ERROR_PROCESS_ABORTED);
            error = L"无法启动 FFmpeg 优化任务";
            return VideoTranscodeResult::failed;
        }

        while (WaitForSingleObject(processHandle.get(), 100) == WAIT_TIMEOUT) {
            auto state = control();
            if (state != VideoTranscodeControl::running) {
                TerminateProcess(processHandle.get(), ERROR_CANCELLED);
                WaitForSingleObject(processHandle.get(), 5000);
                return state == VideoTranscodeControl::paused
                    ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
            }
        }
        DWORD exitCode{};
        if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
            error = L"无法读取 FFmpeg 优化任务状态";
            return VideoTranscodeResult::failed;
        }
        return exitCode == 0 ? VideoTranscodeResult::succeeded : VideoTranscodeResult::unsupported;
    }
}

namespace motion::agent
{
    std::vector<VideoTranscodeBackend> video_transcode_backend_order(
        std::vector<VideoTranscodeAdapter> adapters,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        bool adapterProbeSucceeded,
        bool softwareFallbackAllowed)
    {
        std::stable_sort(adapters.begin(), adapters.end(), [](auto const& left, auto const& right) {
            return left.dedicatedVideoMemory > right.dedicatedVideoMemory;
        });
        std::vector<VideoTranscodeBackend> result;
        std::set<uint32_t> seenVendors;
        auto appendVendor = [&](uint32_t vendor) {
            if (!seenVendors.insert(vendor).second) return;
            if (vendor == vendorNvidia) {
                result.push_back(VideoTranscodeBackend::nvidiaCudaNvenc);
                result.push_back(VideoTranscodeBackend::nvidiaNvenc);
            } else if (vendor == vendorIntel) {
                result.push_back(VideoTranscodeBackend::intelQsv);
            } else if (vendor == vendorAmd) {
                result.push_back(VideoTranscodeBackend::amdAmf);
            }
        };
        for (auto const& adapter : adapters) appendVendor(adapter.vendorId);
        if (!adapterProbeSucceeded) {
            appendVendor(vendorNvidia);
            appendVendor(vendorIntel);
            appendVendor(vendorAmd);
        }

        auto pixelRate = static_cast<uint64_t>(width) * height * targetFps;
        auto longEdge = (std::max)(width, height);
        auto shortEdge = (std::min)(width, height);
        if (softwareFallbackAllowed && width && height && targetFps && pixelRate <= softwarePixelRateLimit &&
            longEdge <= 2560 && shortEdge <= 1440 && targetFps <= 60) {
            result.push_back(VideoTranscodeBackend::softwareKvazaar);
        }
        return result;
    }

    std::wstring video_transcode_backend_name(VideoTranscodeBackend backend)
    {
        switch (backend) {
        case VideoTranscodeBackend::nvidiaCudaNvenc: return L"NVIDIA NVENC（CUDA）";
        case VideoTranscodeBackend::nvidiaNvenc: return L"NVIDIA NVENC";
        case VideoTranscodeBackend::intelQsv: return L"Intel Quick Sync";
        case VideoTranscodeBackend::amdAmf: return L"AMD AMF";
        case VideoTranscodeBackend::softwareKvazaar: return L"软件 HEVC";
        }
        return L"未知后端";
    }

    VideoTranscodeResult transcode_video(
        fs::path const& ffmpeg,
        fs::path const& source,
        fs::path const& destination,
        uint32_t width,
        uint32_t height,
        uint32_t targetFps,
        std::function<VideoTranscodeControl()> const& control,
        std::wstring& error,
        std::wstring* selectedBackend,
        bool softwareFallbackAllowed)
    {
        if (selectedBackend) selectedBackend->clear();
        std::error_code fileError;
        if (!fs::is_regular_file(ffmpeg, fileError)) {
            error = L"FFmpeg 优化后端未安装";
            return VideoTranscodeResult::unsupported;
        }
        if (!width || !height || !targetFps) {
            error = L"无法验证优化副本规格";
            return VideoTranscodeResult::failed;
        }

        bool adapterProbeSucceeded{};
        auto backends = video_transcode_backend_order(installed_adapters(adapterProbeSucceeded),
            width, height, targetFps, adapterProbeSucceeded, softwareFallbackAllowed);
        if (backends.empty()) {
            error = L"没有可用的硬件编码器；该规格超过软件编码的安全上限";
            return VideoTranscodeResult::unsupported;
        }

        std::wstring attemptedBackends;
        for (auto backend : backends) {
            auto state = control();
            if (state != VideoTranscodeControl::running) {
                return state == VideoTranscodeControl::paused
                    ? VideoTranscodeResult::paused : VideoTranscodeResult::cancelled;
            }
            auto backendName = video_transcode_backend_name(backend);
            if (!attemptedBackends.empty()) attemptedBackends += L"、";
            attemptedBackends += backendName;
            fs::remove(destination, fileError);
            auto arguments = transcode_arguments(
                ffmpeg, source, destination, width, height, targetFps, backend);
            auto result = run_ffmpeg(ffmpeg, arguments, control, error);
            if (result == VideoTranscodeResult::paused || result == VideoTranscodeResult::cancelled ||
                result == VideoTranscodeResult::failed) {
                fs::remove(destination, fileError);
                return result;
            }
            if (result != VideoTranscodeResult::succeeded) {
                fs::remove(destination, fileError);
                continue;
            }

            auto actualSize = fs::file_size(destination, fileError);
            auto sourceSize = fs::file_size(source, fileError);
            auto reasonableGrowth = (std::max)(64'000'000ULL, sourceSize / 5);
            if (fileError || !actualSize || actualSize > sourceSize + reasonableGrowth) {
                fs::remove(destination, fileError);
                continue;
            }
            if (selectedBackend) *selectedBackend = backendName;
            error.clear();
            return VideoTranscodeResult::succeeded;
        }

        error = L"可用的编码后端均不支持该视频规格";
        if (!attemptedBackends.empty()) error += L"（已尝试：" + attemptedBackends + L"）";
        return VideoTranscodeResult::unsupported;
    }
}
