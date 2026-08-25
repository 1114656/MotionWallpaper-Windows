#pragma once

#include <windows.h>

namespace motion
{
    inline void enable_per_monitor_dpi_awareness() noexcept
    {
        // Must run before any HWND or display metric is created. Mixed-DPI
        // coordinates then stay in physical pixels across all monitors.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}
