#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl.h>

#include "ResidencyPolicy.h"
#include "AdapterPolicy.h"
#include "DecodePolicy.h"
#include "DesktopHostPolicy.h"
#include "FrameTiming.h"
#include "FrameScheduler.h"
#include "TransitionPolicy.h"
#include "../MotionWallpaper.Common/DisplayAwareness.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include "../MotionWallpaper.Common/UniqueHandle.h"
#include "../MotionWallpaper.Common/TextEncoding.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace
{
    constexpr UINT wmMediaEvent = WM_APP + 1;
    constexpr UINT wmRendererCommand = WM_APP + 2;
    constexpr UINT wmFrameTick = WM_APP + 3;
    constexpr UINT_PTR residencyTimer = 2;

    using motion::renderer::Command;
    using motion::renderer::PresentationMode;
    enum class PlaybackState { Starting, Playing, Freezing, Frozen, Pausing, Paused };
    enum class FrameResult { NoFrame, Presented, Fatal };

    HWND videoWindow{};
    bool hiddenRenderer{};
    std::wstring displayMode{ L"primary" };
    std::vector<std::wstring> monitorDeviceNames;
    PresentationMode presentationMode{ PresentationMode::Desktop };
    PlaybackState playbackState{ PlaybackState::Starting };
    uint64_t latestRevision{};
    uint64_t pendingTargetRevision{};
    ComPtr<IMFMediaEngine> engine;
    bool staticMedia{};
    UINT frameIntervalMs{ 4 };
    LONGLONG lastFrameTimestamp{};
    std::wstring sourcePath;
    double resumeTime{};
    bool resumePending{};
    bool returnToDesktopAfterFreeze{};
    bool visualShown{};
    HANDLE lowMemoryNotification{};

    bool low_memory_pressure()
    {
        BOOL lowMemory{};
        return lowMemoryNotification &&
            QueryMemoryResourceNotification(lowMemoryNotification, &lowMemory) && lowMemory;
    }

    motion::renderer::FrameScheduler frameScheduler{ wmFrameTick };

    std::mutex protocolOutputMutex;

    void acknowledge(uint64_t revision, char const* channel, char const* state)
    {
        if (!revision) return;
        std::scoped_lock lock(protocolOutputMutex);
        std::cout << "ack " << channel << ' ' << revision << ' ' << state << '\n' << std::flush;
    }

    void report_error(uint64_t revision, char const* operation, HRESULT error)
    {
        std::scoped_lock lock(protocolOutputMutex);
        std::cout << "error " << revision << ' ' << operation << " 0x" << std::hex
            << static_cast<unsigned long>(error) << std::dec << '\n' << std::flush;
    }

    void report_decode_status(char const* path, char const* reason)
    {
        std::scoped_lock lock(protocolOutputMutex);
        std::cout << "status decode " << path << ' ' << reason << '\n' << std::flush;
    }

    class MediaNotify final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaEngineNotify>
    {
    public:
        explicit MediaNotify(HWND target) : target_(target) {}
        IFACEMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
        {
            UNREFERENCED_PARAMETER(param1);
            PostMessageW(target_, wmMediaEvent, static_cast<WPARAM>(event), static_cast<LPARAM>(param2));
            return S_OK;
        }
    private:
        HWND target_{};
    };

    ComPtr<MediaNotify> mediaNotify;
    ComPtr<IMFDXGIDeviceManager> deviceManager;

    struct RenderLayout
    {
        RECT bounds{};
        std::vector<RECT> regions;
    } displayLayout;

    RenderLayout selected_display_layout()
    {
        std::vector<RECT> absoluteRegions;
        if (displayMode == L"monitor") {
            for (auto const& deviceName : monitorDeviceNames) {
                auto found = motion::find_display_bounds(deviceName);
                if (found) absoluteRegions.push_back(*found);
            }
        } else {
            absoluteRegions.push_back(motion::primary_display_bounds());
        }
        if (absoluteRegions.empty()) return {};

        RenderLayout result;
        result.bounds = absoluteRegions.front();
        for (auto const& region : absoluteRegions) {
            result.bounds.left = (std::min)(result.bounds.left, region.left);
            result.bounds.top = (std::min)(result.bounds.top, region.top);
            result.bounds.right = (std::max)(result.bounds.right, region.right);
            result.bounds.bottom = (std::max)(result.bounds.bottom, region.bottom);
        }
        result.regions.reserve(absoluteRegions.size());
        for (auto region : absoluteRegions) {
            OffsetRect(&region, -result.bounds.left, -result.bounds.top);
            result.regions.push_back(region);
        }
        return result;
    }

    bool usable_desktop_host(HWND window)
    {
        if (!window || !IsWindowVisible(window)) return false;
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return false;
        int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int virtualRight = virtualLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int virtualBottom = virtualTop + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return motion::renderer::usable_desktop_host_bounds(
            true, bounds.left, bounds.top, bounds.right, bounds.bottom,
            virtualLeft, virtualTop, virtualRight, virtualBottom);
    }

    BOOL CALLBACK find_desktop_host(HWND topLevel, LPARAM output)
    {
        HWND view = FindWindowExW(topLevel, nullptr, L"SHELLDLL_DefView", nullptr);
        if (!view) return TRUE;
        HWND worker = FindWindowExW(nullptr, topLevel, L"WorkerW", nullptr);
        if (!usable_desktop_host(worker)) return TRUE;
        *reinterpret_cast<HWND*>(output) = worker;
        return FALSE;
    }

    HWND existing_desktop_host(HWND progman)
    {
        HWND result{};
        EnumWindows(find_desktop_host, reinterpret_cast<LPARAM>(&result));
        if (result) return result;
        if (HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
            usable_desktop_host(worker)) return worker;
        return nullptr;
    }

    HWND desktop_host()
    {
        HWND progman = FindWindowW(L"Progman", nullptr);
        if (!progman) return nullptr;
        // Explorer normally keeps the wallpaper WorkerW alive for the whole
        // session. Reuse it before asking Progman to create another desktop
        // host; sending 0x052C on every media switch forces an avoidable shell
        // composition refresh that is visible as a short application flash.
        if (HWND existing = existing_desktop_host(progman)) return existing;
        DWORD_PTR ignored{};
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &ignored);
        if (HWND created = existing_desktop_host(progman)) return created;
        return usable_desktop_host(progman) ? progman : nullptr;
    }

    class Presenter
    {
    public:
        bool Initialize(HWND window, std::vector<RECT> regions, std::wstring const& preferredDisplay,
            bool softwareRendering, bool preferHighPerformance)
        {
            if (regions.empty() || !CreateDevice(preferredDisplay, softwareRendering, preferHighPerformance)) return false;
            std::cerr << "adapter " << std::hex << adapterLuid_.HighPart << ':' << adapterLuid_.LowPart
            << std::dec << ' ' << motion::utf8_from_wide(adapterName_) << '\n' << std::flush;

            ComPtr<IDXGIDevice> dxgiDevice;
            if (FAILED(device_.As(&dxgiDevice))) return false;
            if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&composition_)))) return false;
            if (FAILED(composition_->CreateTargetForHwnd(window, TRUE, &target_))) return false;
            if (FAILED(composition_->CreateVisual(&rootVisual_))) return false;
            if (FAILED(target_->SetRoot(rootVisual_.Get()))) return false;

            outputs_.reserve(regions.size());
            for (auto const& region : regions) {
                Output output;
                output.region = region;
                output.width = static_cast<UINT>(region.right - region.left);
                output.height = static_cast<UINT>(region.bottom - region.top);
                if (!output.width || !output.height ||
                    FAILED(composition_->CreateVisual(&output.visual)) ||
                    FAILED(output.visual->SetOffsetX(static_cast<float>(region.left))) ||
                    FAILED(output.visual->SetOffsetY(static_cast<float>(region.top))) ||
                    FAILED(rootVisual_->AddVisual(output.visual.Get(), FALSE, nullptr))) return false;
                outputs_.push_back(std::move(output));
            }
            if (FAILED(composition_->Commit())) return false;
            for (auto& output : outputs_) {
                if (!EnsureSwapChain(output)) return false;
            }
            return true;
        }

        ID3D11Device* Device() const { return device_.Get(); }
        HRESULT LastError() const { return lastError_; }
        LONGLONG LastTimestamp() const { return lastTimestamp_; }

        FrameResult PresentFrame(IMFMediaEngine* mediaEngine, bool captureForFreeze)
        {
            lastError_ = S_OK;
            if (!mediaEngine || outputs_.empty()) { lastError_ = E_POINTER; return FrameResult::Fatal; }
            LONGLONG timestamp{};
            HRESULT result = mediaEngine->OnVideoStreamTick(&timestamp);
            if (result == S_FALSE) return FrameResult::NoFrame;
            if (FAILED(result)) { lastError_ = result; return FrameResult::Fatal; }

            DWORD sourceWidth{}, sourceHeight{};
            if (FAILED(mediaEngine->GetNativeVideoSize(&sourceWidth, &sourceHeight)) || !sourceWidth || !sourceHeight) {
                lastError_ = E_FAIL;
                return FrameResult::Fatal;
            }
            MFARGB black{};
            black.rgbAlpha = 255;
            bool compositionChanged{};
            bool waitForFrozenHandoff{};
            for (auto& output : outputs_) {
                if (!EnsureSwapChain(output)) { lastError_ = E_FAIL; return FrameResult::Fatal; }
                ComPtr<ID3D11Texture2D> buffer;
                result = output.swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
                if (FAILED(result)) { lastError_ = result; return FrameResult::Fatal; }
                auto source = CoverSource(sourceWidth, sourceHeight, output.width, output.height);
                RECT destination{ 0, 0, static_cast<LONG>(output.width), static_cast<LONG>(output.height) };
                result = mediaEngine->TransferVideoFrame(buffer.Get(), &source, &destination, &black);
                if (FAILED(result)) { lastError_ = result; return FrameResult::Fatal; }
                if (captureForFreeze && !CaptureFrame(output, buffer.Get())) output.frozenSurface.Reset();
                result = output.swapChain->Present(1, 0);
                if (FAILED(result)) { lastError_ = result; return FrameResult::Fatal; }
                if (!PrepareSwapChain(output, compositionChanged, waitForFrozenHandoff)) {
                    lastError_ = E_FAIL;
                    return FrameResult::Fatal;
                }
            }
            if (compositionChanged) {
                if (FAILED(composition_->Commit()) ||
                    (waitForFrozenHandoff && FAILED(composition_->WaitForCommitCompletion()))) {
                    lastError_ = E_FAIL;
                    return FrameResult::Fatal;
                }
                for (auto& output : outputs_) output.frozenSurface.Reset();
            }
            lastTimestamp_ = timestamp;
            return FrameResult::Presented;
        }

        bool Compact()
        {
            if (!composition_ || outputs_.empty()) return false;
            for (auto const& output : outputs_) {
                if (!output.frozenSurface) return false;
            }
            for (auto& output : outputs_) {
                if (FAILED(output.visual->SetContent(output.frozenSurface.Get()))) return false;
            }
            if (FAILED(composition_->Commit()) || FAILED(composition_->WaitForCommitCompletion())) return false;
            for (auto& output : outputs_) {
                output.content = Content::FrozenSurface;
                output.swapChain.Reset();
            }
            context_->ClearState();
            context_->Flush();
            Trim();
            return true;
        }

        void Trim()
        {
            ComPtr<IDXGIDevice3> device;
            if (SUCCEEDED(device_.As(&device))) device->Trim();
        }

        uint64_t EstimatedPresenterBytes() const
        {
            uint64_t total{};
            for (auto const& output : outputs_) {
                uint64_t frame = static_cast<uint64_t>(output.width) * output.height * 4;
                total += frame * (output.swapChain ? 2 : output.frozenSurface ? 1 : 0);
            }
            return total;
        }

        void Shutdown()
        {
            for (auto& output : outputs_) {
                output.frozenSurface.Reset();
                output.swapChain.Reset();
                output.visual.Reset();
            }
            outputs_.clear();
            rootVisual_.Reset();
            target_.Reset();
            composition_.Reset();
            if (context_) {
                context_->ClearState();
                context_->Flush();
            }
            context_.Reset();
            device_.Reset();
        }

        bool PresentImage(std::wstring const& source)
        {
            if (outputs_.empty()) return false;
            ComPtr<IWICImagingFactory> imaging;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging)))) return false;
            ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(imaging->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) return false;
            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame))) return false;

            UINT sourceWidth{}, sourceHeight{};
            if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) || !sourceWidth || !sourceHeight) return false;
            constexpr float clear[4]{};
            for (auto& output : outputs_) {
                if (!EnsureSwapChain(output)) return false;
                ComPtr<ID3D11Texture2D> buffer;
                if (FAILED(output.swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
                ComPtr<ID3D11RenderTargetView> targetView;
                if (FAILED(device_->CreateRenderTargetView(buffer.Get(), nullptr, &targetView))) return false;
                context_->ClearRenderTargetView(targetView.Get(), clear);
                auto crop = CoverCrop(sourceWidth, sourceHeight, output.width, output.height);
                ComPtr<IWICBitmapClipper> clipper;
                ComPtr<IWICBitmapScaler> scaler;
                ComPtr<IWICFormatConverter> converter;
                if (FAILED(imaging->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(frame.Get(), &crop))) return false;
                if (FAILED(imaging->CreateBitmapScaler(&scaler)) ||
                    FAILED(scaler->Initialize(clipper.Get(), output.width, output.height, WICBitmapInterpolationModeFant))) return false;
                if (FAILED(imaging->CreateFormatConverter(&converter)) ||
                    FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                        nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;

                UINT stride = output.width * 4;
                std::vector<BYTE> pixels(static_cast<size_t>(stride) * output.height);
                if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
                context_->UpdateSubresource(buffer.Get(), 0, nullptr, pixels.data(), stride, 0);
                if (!CaptureFrame(output, buffer.Get()) || FAILED(output.swapChain->Present(1, 0))) return false;
            }
            return Compact();
        }

    private:
        enum class Content { None, SwapChain, FrozenSurface };

        struct Output
        {
            RECT region{};
            UINT width{};
            UINT height{};
            ComPtr<IDXGISwapChain2> swapChain;
            ComPtr<IDCompositionVisual> visual;
            ComPtr<IDCompositionSurface> frozenSurface;
            Content content{ Content::None };
        };

        static MFVideoNormalizedRect CoverSource(UINT sourceWidth, UINT sourceHeight, UINT targetWidth, UINT targetHeight)
        {
            MFVideoNormalizedRect source{ 0.0f, 0.0f, 1.0f, 1.0f };
            double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
            double targetAspect = static_cast<double>(targetWidth) / targetHeight;
            if (sourceAspect > targetAspect) {
                float visible = static_cast<float>(targetAspect / sourceAspect);
                source.left = (1.0f - visible) * 0.5f;
                source.right = source.left + visible;
            } else if (sourceAspect < targetAspect) {
                float visible = static_cast<float>(sourceAspect / targetAspect);
                source.top = (1.0f - visible) * 0.5f;
                source.bottom = source.top + visible;
            }
            return source;
        }

        static WICRect CoverCrop(UINT sourceWidth, UINT sourceHeight, UINT targetWidth, UINT targetHeight)
        {
            double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
            double targetAspect = static_cast<double>(targetWidth) / targetHeight;
            WICRect crop{ 0, 0, static_cast<INT>(sourceWidth), static_cast<INT>(sourceHeight) };
            if (sourceAspect > targetAspect) {
                crop.Width = static_cast<INT>(sourceHeight * targetAspect);
                crop.X = (static_cast<INT>(sourceWidth) - crop.Width) / 2;
            } else if (sourceAspect < targetAspect) {
                crop.Height = static_cast<INT>(sourceWidth / targetAspect);
                crop.Y = (static_cast<INT>(sourceHeight) - crop.Height) / 2;
            }
            return crop;
        }

        bool EnsureSwapChain(Output& output)
        {
            if (output.swapChain) return true;
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
                FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = output.width;
            description.Height = output.height;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            ComPtr<IDXGISwapChain1> swapChain;
            if (FAILED(factory->CreateSwapChainForComposition(device_.Get(), &description, nullptr, &swapChain)) ||
                FAILED(swapChain.As(&output.swapChain))) return false;
            output.swapChain->SetMaximumFrameLatency(1);
            return true;
        }

        bool PrepareSwapChain(Output& output, bool& compositionChanged, bool& waitForFrozenHandoff)
        {
            if (output.content == Content::SwapChain) return true;
            if (!output.swapChain || FAILED(output.visual->SetContent(output.swapChain.Get()))) return false;
            compositionChanged = true;
            waitForFrozenHandoff = waitForFrozenHandoff || output.content == Content::FrozenSurface;
            output.content = Content::SwapChain;
            return true;
        }

        bool CaptureFrame(Output& output, ID3D11Texture2D* source)
        {
            if (!source) return false;
            ComPtr<IDCompositionSurface> captured;
            if (FAILED(composition_->CreateSurface(output.width, output.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_IGNORE, &captured))) return false;

            ComPtr<IDXGISurface> update;
            POINT offset{};
            if (FAILED(captured->BeginDraw(nullptr, IID_PPV_ARGS(&update), &offset))) return false;
            ComPtr<ID3D11Texture2D> destination;
            HRESULT result = update.As(&destination);
            if (SUCCEEDED(result)) {
                D3D11_BOX sourceBox{ 0, 0, 0, output.width, output.height, 1 };
                context_->CopySubresourceRegion(destination.Get(), 0,
                    static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
                    source, 0, &sourceBox);
                context_->Flush();
            }
            HRESULT endResult = captured->EndDraw();
            if (FAILED(result) || FAILED(endResult)) return false;
            output.frozenSurface = std::move(captured);
            return true;
        }

        static int AdapterRank(IDXGIAdapter1* adapter, std::wstring const& preferredDisplay)
        {
            MONITORINFOEXW primary{ sizeof(primary) };
            GetMonitorInfoW(MonitorFromPoint({}, MONITOR_DEFAULTTOPRIMARY), &primary);
            auto const preferred = preferredDisplay.empty() ? std::wstring(primary.szDevice) : preferredDisplay;
            bool hasDesktopOutput = false;
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(index, &output) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC description{};
                if (FAILED(output->GetDesc(&description)) || !description.AttachedToDesktop) continue;
                hasDesktopOutput = true;
                if (_wcsicmp(description.DeviceName, preferred.c_str()) == 0) return 0;
            }
            return hasDesktopOutput ? 1 : 2;
        }

        bool CreateDevice(std::wstring const& preferredDisplay, bool softwareRendering, bool preferHighPerformance)
        {
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            if (!softwareRendering) flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            if (softwareRendering) {
                D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_);
            } else {
                ComPtr<IDXGIFactory6> factory;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
                std::vector<ComPtr<IDXGIAdapter1>> adapters;
                auto preference = preferHighPerformance ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE :
                    DXGI_GPU_PREFERENCE_MINIMUM_POWER;
                for (UINT index = 0;; ++index) {
                    ComPtr<IDXGIAdapter1> adapter;
                    if (factory->EnumAdapterByGpuPreference(index, preference, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
                    DXGI_ADAPTER_DESC1 description{};
                    adapter->GetDesc1(&description);
                    if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                    adapters.push_back(std::move(adapter));
                }
                std::stable_sort(adapters.begin(), adapters.end(), [&](auto const& left, auto const& right) {
                    return AdapterRank(left.Get(), preferredDisplay) < AdapterRank(right.Get(), preferredDisplay);
                });
                for (auto const& adapter : adapters) {
                    if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_))) break;
                }
            }
            if (!device_) return false;
            ComPtr<ID3D10Multithread> multithread;
            if (SUCCEEDED(device_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> baseAdapter;
            ComPtr<IDXGIAdapter1> adapter;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&baseAdapter)) ||
                FAILED(baseAdapter.As(&adapter)) || FAILED(adapter->GetDesc1(&description))) return false;
            adapterName_ = description.Description;
            adapterLuid_ = description.AdapterLuid;
            return true;
        }

        std::vector<Output> outputs_;
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<IDCompositionDevice> composition_;
        ComPtr<IDCompositionTarget> target_;
        ComPtr<IDCompositionVisual> rootVisual_;
        std::wstring adapterName_;
        LUID adapterLuid_{};
        HRESULT lastError_{ S_OK };
        LONGLONG lastTimestamp_{};
    } presenter;

    bool frame_rendering_active()
    {
        return playbackState == PlaybackState::Playing || playbackState == PlaybackState::Starting ||
            playbackState == PlaybackState::Freezing || playbackState == PlaybackState::Pausing;
    }

    void start_frame_timer() { frameScheduler.Start(videoWindow, frameIntervalMs); }
    void stop_frame_timer() { frameScheduler.Stop(); }
    void cancel_residency_timer() { KillTimer(videoWindow, residencyTimer); }
    void enter_idle_residency()
    {
        if (staticMedia) return;
        SetTimer(videoWindow, residencyTimer,
            motion::renderer::residency_timer_delay_ms(low_memory_pressure()), nullptr);
    }
    void leave_idle_residency()
    {
        cancel_residency_timer();
    }

    void release_decoder()
    {
        if (!engine) return;
        resumeTime = engine->GetCurrentTime();
        engine->Pause();
        engine->Shutdown();
        engine.Reset();
        deviceManager.Reset();
        mediaNotify.Reset();
        resumePending = resumeTime > 0.0;
        std::cerr << "decoder released " << resumeTime << '\n' << std::flush;
    }

    void compact_idle_resources()
    {
        cancel_residency_timer();
        release_decoder();
        bool compacted = presenter.Compact();
        std::cerr << "residency " << (compacted ? "compact" : "decoder-only")
            << " presenter-mib " << (presenter.EstimatedPresenterBytes() / (1024 * 1024))
            << '\n' << std::flush;
    }

    void adapt_frame_timer()
    {
        auto timestamp = presenter.LastTimestamp();
        if (lastFrameTimestamp && timestamp > lastFrameTimestamp) {
            auto duration = timestamp - lastFrameTimestamp;
            if (duration >= 40'000 && duration <= 1'000'000) {
                frameIntervalMs = motion::renderer::presentation_probe_interval_ms(duration);
            }
        }
        lastFrameTimestamp = timestamp;
    }

    bool create_engine(std::wstring const& source);

    void show_renderer_window()
    {
        if (hiddenRenderer || visualShown || !videoWindow) return;
        ShowWindow(videoWindow, SW_SHOWNOACTIVATE);
        SetWindowPos(videoWindow,
            presentationMode == PresentationMode::Screensaver ? HWND_TOPMOST : HWND_BOTTOM,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        visualShown = true;
    }

    bool apply_window_region()
    {
        if (displayLayout.regions.size() <= 1) return SetWindowRgn(videoWindow, nullptr, TRUE) != 0;
        HRGN combined = CreateRectRgn(0, 0, 0, 0);
        if (!combined) return false;
        for (auto const& region : displayLayout.regions) {
            HRGN part = CreateRectRgn(region.left, region.top, region.right, region.bottom);
            if (!part || CombineRgn(combined, combined, part, RGN_OR) == ERROR) {
                if (part) DeleteObject(part);
                DeleteObject(combined);
                return false;
            }
            DeleteObject(part);
        }
        if (!SetWindowRgn(videoWindow, combined, TRUE)) {
            DeleteObject(combined);
            return false;
        }
        return true;
    }

    bool size_window(PresentationMode requested)
    {
        if (hiddenRenderer || presentationMode == requested) return true;
        UINT flags = SWP_NOACTIVATE | SWP_FRAMECHANGED;
        if (visualShown) flags |= SWP_SHOWWINDOW;
        auto bounds = displayLayout.bounds;
        int x = bounds.left;
        int y = bounds.top;
        int width = bounds.right - bounds.left;
        int height = bounds.bottom - bounds.top;
        if (requested == PresentationMode::Screensaver) {
            SetParent(videoWindow, nullptr);
            SetWindowLongPtrW(videoWindow, GWL_STYLE, WS_POPUP | (visualShown ? WS_VISIBLE : 0));
            if (!SetWindowPos(videoWindow, HWND_TOPMOST, x, y, width, height, flags)) return false;
        } else {
            HWND host = desktop_host();
            if (!host) { SetLastError(ERROR_NOT_FOUND); return false; }
            SetParent(videoWindow, host);
            SetWindowLongPtrW(videoWindow, GWL_STYLE, WS_CHILD | (visualShown ? WS_VISIBLE : 0));
            int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            if (!SetWindowPos(videoWindow, HWND_BOTTOM, x - virtualX, y - virtualY, width, height, flags)) return false;
        }
        if (!apply_window_region()) return false;
        presentationMode = requested;
        return true;
    }

    void render_tick()
    {
        if (!engine) return;
        bool captureForFreeze = playbackState == PlaybackState::Freezing || playbackState == PlaybackState::Pausing;
        auto result = presenter.PresentFrame(engine.Get(), captureForFreeze);
        if (result == FrameResult::NoFrame) {
            if (frame_rendering_active()) frameScheduler.Start(videoWindow, 2);
            return;
        }
        if (result == FrameResult::Fatal) {
            report_error(pendingTargetRevision, "present", presenter.LastError());
            PostMessageW(videoWindow, WM_CLOSE, 0, 0);
            return;
        }
        adapt_frame_timer();
        show_renderer_window();
        if (playbackState == PlaybackState::Freezing || playbackState == PlaybackState::Pausing) {
            bool pausing = playbackState == PlaybackState::Pausing;
            engine->Pause();
            playbackState = pausing ? PlaybackState::Paused : PlaybackState::Frozen;
            stop_frame_timer();
            if (returnToDesktopAfterFreeze) {
                returnToDesktopAfterFreeze = false;
                if (!size_window(PresentationMode::Desktop)) {
                    report_error(pendingTargetRevision, "desktop-host", HRESULT_FROM_WIN32(GetLastError()));
                    PostMessageW(videoWindow, WM_CLOSE, 0, 0);
                    return;
                }
            }
            enter_idle_residency();
            acknowledge(pendingTargetRevision, "target", pausing ? "paused" : "frozen");
        } else if (playbackState == PlaybackState::Starting) {
            playbackState = PlaybackState::Playing;
            acknowledge(pendingTargetRevision, "target", "playing");
        }
        if (frame_rendering_active()) start_frame_timer();
    }

    void apply_command(Command command, uint64_t revision)
    {
        if (revision < latestRevision) return;
        latestRevision = revision;
        if (command == Command::Stop) {
            acknowledge(revision, "control", "stopping");
            PostMessageW(videoWindow, WM_CLOSE, 0, 0);
            return;
        }
        pendingTargetRevision = revision;
        if (command == Command::Pause) {
            bool returnToDesktop = motion::renderer::leaves_screensaver(presentationMode, command);
            if (staticMedia || !engine || playbackState == PlaybackState::Frozen || playbackState == PlaybackState::Paused) {
                playbackState = PlaybackState::Paused;
                stop_frame_timer();
                if (engine) engine->Pause();
                if (returnToDesktop && !size_window(PresentationMode::Desktop)) {
                    report_error(revision, "desktop-host", HRESULT_FROM_WIN32(GetLastError()));
                    PostMessageW(videoWindow, WM_CLOSE, 0, 0);
                    return;
                }
                returnToDesktopAfterFreeze = false;
                enter_idle_residency();
                acknowledge(revision, "target", "paused");
            } else {
                returnToDesktopAfterFreeze = returnToDesktop;
                playbackState = PlaybackState::Pausing;
                engine->Play();
                start_frame_timer();
            }
            return;
        }

        returnToDesktopAfterFreeze = false;

        if (staticMedia) {
            PresentationMode requested = command == Command::ScreensaverPlay ? PresentationMode::Screensaver : PresentationMode::Desktop;
            if (!size_window(requested)) {
                report_error(revision, "desktop-host", HRESULT_FROM_WIN32(GetLastError()));
                PostMessageW(videoWindow, WM_CLOSE, 0, 0);
                return;
            }
            show_renderer_window();
            acknowledge(revision, "target", command == Command::DesktopFreeze ? "frozen" : "playing");
            return;
        }

        if (command == Command::DesktopFreeze && presentationMode == PresentationMode::Screensaver) {
            leave_idle_residency();
            if (!engine && !create_engine(sourcePath)) {
                report_error(revision, "decoder-resume", E_FAIL);
                PostMessageW(videoWindow, WM_CLOSE, 0, 0);
                return;
            }
            returnToDesktopAfterFreeze = true;
            playbackState = PlaybackState::Freezing;
            engine->Play();
            start_frame_timer();
            return;
        }

        PresentationMode requested = command == Command::ScreensaverPlay ? PresentationMode::Screensaver : PresentationMode::Desktop;
        if (!size_window(requested)) {
            report_error(revision, "desktop-host", HRESULT_FROM_WIN32(GetLastError()));
            PostMessageW(videoWindow, WM_CLOSE, 0, 0);
            return;
        }
        leave_idle_residency();
        if (!engine && !create_engine(sourcePath)) {
            report_error(revision, "decoder-resume", E_FAIL);
            PostMessageW(videoWindow, WM_CLOSE, 0, 0);
            return;
        }
        playbackState = command == Command::DesktopFreeze ? PlaybackState::Freezing : PlaybackState::Starting;
        if (engine) engine->Play();
        start_frame_timer();
    }

    void handle_media_event(DWORD event, DWORD status)
    {
        if (FAILED(static_cast<HRESULT>(status))) {
            report_error(pendingTargetRevision, "media", static_cast<HRESULT>(status));
            PostMessageW(videoWindow, WM_CLOSE, 0, 0);
            return;
        }
        switch (event) {
        case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
        case MF_MEDIA_ENGINE_EVENT_PLAYING:
            if (frame_rendering_active()) {
                start_frame_timer();
                render_tick();
            }
            break;
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
            if (engine && resumePending) {
                engine->SetCurrentTime(resumeTime);
                resumePending = false;
            }
            if (engine && (playbackState == PlaybackState::Starting || playbackState == PlaybackState::Freezing ||
                playbackState == PlaybackState::Pausing)) engine->Play();
            break;
        }
    }

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_PAINT: ValidateRect(window, nullptr); return 0;
        case WM_ERASEBKGND: return 1;
        case wmFrameTick:
            frameScheduler.TickHandled();
            if (frame_rendering_active()) render_tick();
            return 0;
        case WM_TIMER:
            if (wParam == residencyTimer) {
                if (motion::renderer::should_compact_idle(low_memory_pressure())) compact_idle_resources();
                return 0;
            }
            break;
        case wmMediaEvent:
            handle_media_event(static_cast<DWORD>(wParam), static_cast<DWORD>(lParam));
            return 0;
        case wmRendererCommand:
            apply_command(static_cast<Command>(wParam), static_cast<uint64_t>(lParam));
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            // The Agent will relaunch us with new physical monitor bounds and,
            // when needed, a different display-attached GPU.
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY: stop_frame_timer(); cancel_residency_timer(); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool register_window_class(HINSTANCE instance)
    {
        WNDCLASSEXW definition{ sizeof(definition) };
        definition.lpfnWndProc = window_proc;
        definition.hInstance = instance;
        definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        definition.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        definition.lpszClassName = L"MotionWallpaper.Native.Renderer";
        if (!RegisterClassExW(&definition) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        return true;
    }

    bool video_prefers_high_performance_adapter(std::wstring const& source)
    {
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader))) return false;
        ComPtr<IMFMediaType> mediaType;
        if (FAILED(reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &mediaType))) return false;
        UINT32 width{}, height{}, frameRateNumerator{}, frameRateDenominator{};
        if (FAILED(MFGetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
            FAILED(MFGetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, &frameRateNumerator, &frameRateDenominator))) {
            return false;
        }
        bool highPerformance = motion::renderer::prefer_high_performance_adapter(
            width, height, frameRateNumerator, frameRateDenominator);
        std::cerr << "workload " << width << 'x' << height << ' ' << frameRateNumerator << '/'
            << frameRateDenominator << " adapter-policy " << (highPerformance ? "performance" : "efficiency")
            << '\n' << std::flush;
        return highPerformance;
    }

    bool has_hardware_decoder(std::wstring const& source)
    {
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader))) return false;
        ComPtr<IMFMediaType> mediaType;
        if (FAILED(reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &mediaType))) return false;
        GUID subtype{};
        if (FAILED(mediaType->GetGUID(MF_MT_SUBTYPE, &subtype))) return false;

        MFT_REGISTER_TYPE_INFO inputType{ MFMediaType_Video, subtype };
        IMFActivate** activations{};
        UINT32 count{};
        HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType, nullptr, &activations, &count);
        if (activations) {
            for (UINT32 index = 0; index < count; ++index) {
                if (activations[index]) activations[index]->Release();
            }
            CoTaskMemFree(activations);
        }
        return SUCCEEDED(result) && count != 0;
    }

    bool create_window(bool desktop, bool hidden, bool softwareRendering, bool preferHighPerformance)
    {
        hiddenRenderer = hidden;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!register_window_class(instance)) return false;
        HWND parent = desktop ? desktop_host() : nullptr;
        if (desktop && !parent) return false;
        presentationMode = desktop ? PresentationMode::Desktop : PresentationMode::Screensaver;
        DWORD style = desktop ? WS_CHILD : hidden ? WS_OVERLAPPED : WS_POPUP;
        displayLayout = selected_display_layout();
        auto bounds = displayLayout.bounds;
        int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int x = desktop ? bounds.left - virtualX : bounds.left;
        int y = desktop ? bounds.top - virtualY : bounds.top;
        int width = bounds.right - bounds.left;
        int height = bounds.bottom - bounds.top;
        std::cerr << "display " << motion::utf8_from_wide(displayMode) << ' ' << displayLayout.regions.size()
            << " targets " << width << 'x' << height
            << " origin " << bounds.left << ',' << bounds.top << " physical-pixels\n" << std::flush;
        videoWindow = CreateWindowExW(0, L"MotionWallpaper.Native.Renderer", L"MotionWallpaper Renderer", style, x, y, width, height, parent, nullptr, instance, nullptr);
        std::wstring preferredDisplay = monitorDeviceNames.empty() ? std::wstring{} : monitorDeviceNames.front();
        return videoWindow && apply_window_region() && presenter.Initialize(
            videoWindow, displayLayout.regions, preferredDisplay, softwareRendering, preferHighPerformance);
    }

    bool create_engine(std::wstring const& source)
    {
        ComPtr<IMFAttributes> attributes;
        if (FAILED(MFCreateAttributes(&attributes, 4))) return false;
        mediaNotify = Make<MediaNotify>(videoWindow);
        if (!mediaNotify) return false;
        if (FAILED(attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, mediaNotify.Get()))) return false;
        UINT resetToken{};
        if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &deviceManager))) return false;
        if (FAILED(deviceManager->ResetDevice(presenter.Device(), resetToken))) return false;
        if (FAILED(attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, deviceManager.Get()))) return false;
        if (FAILED(attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM))) return false;
        ComPtr<IMFMediaEngineClassFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
        // Local wallpaper playback is latency-sensitive and does not benefit
        // from a large read-ahead queue. Real-time mode bounds buffering while
        // preserving source resolution and decoder choice.
        DWORD flags = MF_MEDIA_ENGINE_FORCEMUTE | MF_MEDIA_ENGINE_REAL_TIME_MODE;
        if (FAILED(factory->CreateInstance(flags, attributes.Get(), &engine))) return false;
        engine->SetMuted(TRUE);
        engine->SetLoop(TRUE);
        engine->SetAutoPlay(FALSE);
        BSTR path = SysAllocString(source.c_str());
        if (!path) return false;
        HRESULT result = engine->SetSource(path);
        SysFreeString(path);
        if (FAILED(result)) return false;
        lastFrameTimestamp = 0;
        frameIntervalMs = 4;
        return true;
    }

    Command parse_command(std::string const& name)
    {
        if (name == "desktop-play") return Command::DesktopPlay;
        if (name == "desktop-freeze") return Command::DesktopFreeze;
        if (name == "screensaver-play") return Command::ScreensaverPlay;
        if (name == "pause") return Command::Pause;
        if (name == "stop") return Command::Stop;
        return Command::Unknown;
    }

    bool apply_protocol_test_command(Command command, uint64_t revision)
    {
        if (revision < latestRevision) return false;
        latestRevision = revision;
        if (command == Command::Stop) {
            acknowledge(revision, "control", "stopping");
            return true;
        }
        auto state = command == Command::Pause ? "paused" :
            command == Command::DesktopFreeze ? "frozen" : "playing";
        acknowledge(revision, "target", state);
        return false;
    }

    void command_reader(bool protocolTest = false)
    {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream input(line);
            std::string name;
            uint64_t revision{};
            input >> name >> revision;
            if (name.empty()) continue;
            if (!revision) {
                report_error(0, "missing-revision", E_INVALIDARG);
                continue;
            }
            Command command = parse_command(name);
            if (command == Command::Unknown) {
                report_error(revision, "invalid-command", E_INVALIDARG);
                continue;
            }
            if (protocolTest) {
                if (apply_protocol_test_command(command, revision)) return;
                continue;
            }
            if (videoWindow) PostMessageW(videoWindow, wmRendererCommand, static_cast<WPARAM>(command), static_cast<LPARAM>(revision));
            if (command == Command::Stop) return;
        }
        if (videoWindow) PostMessageW(videoWindow, WM_CLOSE, 0, 0);
    }
}

int wmain(int argc, wchar_t** argv)
{
    motion::enable_per_monitor_dpi_awareness();
    std::wstring video;
    bool desktop = false;
    bool hidden = false;
    bool probe = false;
    bool protocolTest = false;
    std::wstring decodeMode = L"auto";
    std::wstring mediaKind = L"video";
    for (int index = 1; index < argc; ++index) {
        std::wstring argument = argv[index];
        if (argument == L"-video" && index + 1 < argc) video = argv[++index];
        else if (argument == L"-desktop") desktop = true;
        else if (argument == L"-hidden") hidden = true;
        else if (argument == L"-probe-desktop") probe = true;
        else if (argument == L"-protocol-test") protocolTest = true;
        else if (argument == L"-decode" && index + 1 < argc) decodeMode = argv[++index];
        else if (argument == L"-kind" && index + 1 < argc) mediaKind = argv[++index];
        else if (argument == L"-display" && index + 1 < argc) displayMode = argv[++index];
        else if (argument == L"-monitor" && index + 1 < argc) monitorDeviceNames.push_back(argv[++index]);
    }
    if (decodeMode != L"auto" && decodeMode != L"hardware" && decodeMode != L"software") return 6;
    if (mediaKind != L"video" && mediaKind != L"image") return 7;
    if (displayMode != L"primary" && displayMode != L"monitor") return 8;
    if (displayMode == L"monitor" && monitorDeviceNames.empty()) return 9;
    if (probe) { if (desktop_host()) { std::cout << "desktop host available\n"; return 0; } return 1; }
    if (protocolTest) { command_reader(true); return 0; }
    if (video.empty() || GetFileAttributesW(video.c_str()) == INVALID_FILE_ATTRIBUTES) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 4; }
    lowMemoryNotification = CreateMemoryResourceNotification(LowMemoryResourceNotification);

    int result = 0;
    staticMedia = mediaKind == L"image";
    sourcePath = video;
    bool hardwareDecoderAvailable = staticMedia ? false : has_hardware_decoder(video);
    auto decodePath = staticMedia
        ? motion::renderer::DecodePath::Software
        : motion::renderer::select_decode_path(decodeMode, hardwareDecoderAvailable);
    if (staticMedia) {
        report_decode_status("not-applicable", "image");
    } else if (decodePath == motion::renderer::DecodePath::Hardware) {
        report_decode_status("hardware", "hardware-decoder-available");
    } else if (decodePath == motion::renderer::DecodePath::Unavailable) {
        report_decode_status("unavailable", "no-hardware-decoder");
        report_error(0, "hardware-decoder-unavailable", HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        if (lowMemoryNotification) { CloseHandle(lowMemoryNotification); lowMemoryNotification = nullptr; }
        MFShutdown();
        CoUninitialize();
        return 10;
    } else if (decodeMode == L"software") {
        report_decode_status("software", "requested-software");
    } else {
        report_decode_status("software-fallback", "fallback-no-hardware-decoder");
    }
    bool softwareRendering = !staticMedia && decodePath == motion::renderer::DecodePath::Software;
    bool preferHighPerformance = !staticMedia && !softwareRendering &&
        video_prefers_high_performance_adapter(video);
    if (!create_window(desktop, hidden, softwareRendering, preferHighPerformance) ||
        (staticMedia ? !presenter.PresentImage(video) : !create_engine(video))) {
        result = 5;
    } else {
        std::thread input(command_reader, false);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (input.joinable()) {
            CancelSynchronousIo(input.native_handle());
            input.join();
        }
    }

    if (engine) { engine->Shutdown(); engine.Reset(); }
    deviceManager.Reset();
    mediaNotify.Reset();
    presenter.Shutdown();
    if (lowMemoryNotification) { CloseHandle(lowMemoryNotification); lowMemoryNotification = nullptr; }
    MFShutdown();
    CoUninitialize();
    return result;
}
