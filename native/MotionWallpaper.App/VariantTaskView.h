#pragma once

#include "VariantPageModel.h"

#include <functional>

namespace motion::app
{
    using VariantPauseAction = std::function<void(bool paused)>;
    using VariantCancelAction = std::function<void()>;

    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::Border create_variant_task_card(
        VariantMediaSummary const& item,
        std::filesystem::path const& cover,
        bool waitingForPower,
        VariantPauseAction pause,
        VariantCancelAction cancel);
}
