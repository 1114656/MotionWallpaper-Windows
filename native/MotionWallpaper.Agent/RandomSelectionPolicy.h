#pragma once

namespace motion::agent
{
    enum class RandomSelectionAction
    {
        UseSelected,
        KeepRandom,
        ChooseRandom
    };

    inline RandomSelectionAction random_selection_action(
        bool active,
        bool groupChanged,
        bool selectionChanged,
        bool unlocked,
        bool intervalDue,
        bool currentRandomIsValid) noexcept
    {
        if (!active) return RandomSelectionAction::UseSelected;
        if (groupChanged) return RandomSelectionAction::ChooseRandom;
        if (selectionChanged) return RandomSelectionAction::UseSelected;
        if (unlocked || intervalDue || !currentRandomIsValid) return RandomSelectionAction::ChooseRandom;
        return RandomSelectionAction::KeepRandom;
    }
}
