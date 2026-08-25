#pragma once

#include "App.xaml.g.h"
#include "../MotionWallpaper.Common/Common.h"

#include <thread>

namespace winrt::MotionWallpaper::implementation
{
    struct App : AppT<App>
    {
        App();
        ~App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        motion::unique_handle instanceMutex;
        motion::unique_handle exitEvent;
        motion::unique_handle exitStopEvent;
        std::thread exitWatcher;
        Microsoft::UI::Xaml::Window window{ nullptr };
    };
}
