#pragma once

#include <string>

struct DigitalVibranceState
{
    int currentLevel = 0;
    int minimumLevel = 0;
    int maximumLevel = 100;
    int defaultLevel = 50;
};

class NvDigitalVibrance
{
public:
    static bool Query(const std::wstring& gdiDeviceName,
                      DigitalVibranceState& state, std::wstring& error);
    static bool Set(const std::wstring& gdiDeviceName, int level,
                    DigitalVibranceState& resultingState, std::wstring& error);
};
