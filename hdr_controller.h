#pragma once

#include <Windows.h>

#include <string>

struct HdrDisplayState
{
    LUID adapterId{};
    UINT32 targetId = 0;
    std::wstring gdiDeviceName;
    std::wstring friendlyName;
    bool hdrSupported = false;
    bool hdrEnabled = false;
    bool usesModernHdrApi = false;
};

class HdrController
{
public:
    static bool QueryPrimaryDisplay(HdrDisplayState& state, std::wstring& error);
    static bool SetEnabled(const HdrDisplayState& display, bool enabled,
                           HdrDisplayState& resultingState, std::wstring& error);
};
