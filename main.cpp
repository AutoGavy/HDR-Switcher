#include "hdr_controller.h"
#include "nv_digital_vibrance.h"

#include <chrono>
#include <string>
#include <thread>

namespace
{
constexpr int kHdrOnVibrance = 60;
constexpr int kHdrOffVibrance = 50;

bool SetDvcWithRetry(const std::wstring& deviceName, int level,
                     DigitalVibranceState& state, std::wstring& error)
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (NvDigitalVibrance::Set(deviceName, level, state, error))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
}
} // namespace

int main()
{
    HdrDisplayState before{};
    std::wstring error;
    if (!HdrController::QueryPrimaryDisplay(before, error) ||
        !before.hdrSupported)
    {
        return 1;
    }

    const bool enableHdr = !before.hdrEnabled;
    HdrDisplayState after{};
    if (!HdrController::SetEnabled(before, enableHdr, after, error))
    {
        return 2;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    const std::wstring deviceName = after.gdiDeviceName.empty()
                                        ? before.gdiDeviceName
                                        : after.gdiDeviceName;
    const int targetDvc = enableHdr ? kHdrOnVibrance : kHdrOffVibrance;
    DigitalVibranceState dvc{};
    if (!SetDvcWithRetry(deviceName, targetDvc, dvc, error))
    {
        return 3;
    }

    return 0;
}
