#include "pch.h"
#include "VariantTaskView.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace
{
    std::wstring file_uri(std::filesystem::path const& path)
    {
        return L"file:///" + path.generic_wstring();
    }
}

namespace motion::app
{
    Border create_variant_task_card(VariantMediaSummary const& item,
        std::filesystem::path const& cover, bool waitingForPower,
        VariantPauseAction pauseAction, VariantCancelAction cancelAction)
    {
        auto stroke = Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(255, 220, 226, 232) };
        auto surface = Microsoft::UI::Xaml::Media::SolidColorBrush{ Windows::UI::Colors::White() };
        auto muted = Microsoft::UI::Xaml::Media::SolidColorBrush{
            Windows::UI::ColorHelper::FromArgb(255, 245, 247, 249) };

        Border task;
        task.Padding(ThicknessHelper::FromUniformLength(14));
        task.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
        task.BorderThickness(ThicknessHelper::FromUniformLength(1));
        task.BorderBrush(stroke);
        task.Background(surface);

        Grid layout;
        layout.ColumnSpacing(14);
        for (auto width : { 72.0, 0.0, 130.0, 190.0, -1.0 }) {
            ColumnDefinition column;
            column.Width(width == 0.0
                ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star)
                : width < 0.0 ? GridLengthHelper::Auto()
                : GridLengthHelper::FromPixels(width));
            layout.ColumnDefinitions().Append(column);
        }

        Border preview;
        preview.Width(72);
        preview.Height(46);
        preview.CornerRadius(CornerRadiusHelper::FromUniformRadius(7));
        preview.Background(muted);
        if (!item.media.coverFileName.empty() && std::filesystem::is_regular_file(cover)) {
            Image image;
            image.Stretch(Microsoft::UI::Xaml::Media::Stretch::UniformToFill);
            image.Source(Microsoft::UI::Xaml::Media::Imaging::BitmapImage{
                Windows::Foundation::Uri(file_uri(cover)) });
            preview.Child(image);
        }
        layout.Children().Append(preview);

        StackPanel identity;
        identity.VerticalAlignment(VerticalAlignment::Center);
        TextBlock name;
        name.Text(item.media.name);
        name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        TextBlock group;
        group.Text(item.groupName);
        group.FontSize(12);
        group.Opacity(0.58);
        identity.Children().Append(name);
        identity.Children().Append(group);
        Grid::SetColumn(identity, 1);
        layout.Children().Append(identity);

        TextBlock mode;
        mode.Text(item.status.requestedMode == "power-saver" ? L"低功耗" : L"自动平衡");
        mode.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(mode, 2);
        layout.Children().Append(mode);

        StackPanel state;
        state.Spacing(5);
        state.VerticalAlignment(VerticalAlignment::Center);
        TextBlock stateText;
        if (item.status.paused) stateText.Text(L"已暂停");
        else if (item.status.generating) stateText.Text(L"正在生成");
        else if (waitingForPower) stateText.Text(L"等待接通电源");
        else stateText.Text(L"等待生成");
        stateText.FontSize(12);
        state.Children().Append(stateText);
        if (!item.status.paused && !waitingForPower) {
            ProgressBar progress;
            progress.IsIndeterminate(true);
            progress.Height(3);
            state.Children().Append(progress);
        }
        Grid::SetColumn(state, 3);
        layout.Children().Append(state);

        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(8);
        actions.VerticalAlignment(VerticalAlignment::Center);
        Button pause;
        pause.Content(box_value(item.status.paused ? L"继续" : L"暂停"));
        pause.Click([action = std::move(pauseAction), paused = item.status.paused](auto const&, auto const&) {
            action(!paused);
        });
        Button cancel;
        cancel.Content(box_value(L"取消"));
        cancel.Click([action = std::move(cancelAction)](auto const&, auto const&) { action(); });
        actions.Children().Append(pause);
        actions.Children().Append(cancel);
        Grid::SetColumn(actions, 4);
        layout.Children().Append(actions);

        task.Child(layout);
        return task;
    }
}
