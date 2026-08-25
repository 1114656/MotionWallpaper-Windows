#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::MotionWallpaper::implementation
{
    App::App()
    {
        instanceMutex.reset(CreateMutexW(nullptr, FALSE, L"Local\\MotionWallpaper.SettingsApp"));
        if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                if (HWND existing = FindWindowW(nullptr, L"MotionWallpaper")) {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                    break;
                }
                Sleep(50);
            }
            ExitProcess(0);
        }
        InitializeComponent();
    }

    App::~App()
    {
        if (exitStopEvent) SetEvent(exitStopEvent.get());
        if (exitWatcher.joinable()) exitWatcher.join();
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        window = make<MainWindow>();
        window.Activate();

        exitEvent.reset(CreateEventW(nullptr, TRUE, FALSE, motion::app_exit_event_name));
        exitStopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!exitEvent || !exitStopEvent) return;
        auto dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        exitWatcher = std::thread([this, dispatcher] {
            HANDLE events[]{ exitStopEvent.get(), exitEvent.get() };
            if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
            dispatcher.TryEnqueue([this] {
                if (!window) return;
                window.Close();
                window = nullptr;
            });
        });
    }
}
