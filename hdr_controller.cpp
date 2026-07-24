#include "hdr_controller.h"

#include <chrono>
#include <cwchar>
#include <thread>
#include <vector>

namespace
{
std::wstring FormatSystemError(LONG code)
{
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0,
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);

    std::wstring result;
    if (length != 0 && message != nullptr)
    {
        result.assign(message, length);
        while (!result.empty() &&
               (result.back() == L'\r' || result.back() == L'\n' ||
                result.back() == L' '))
        {
            result.pop_back();
        }
    }
    else
    {
        result = L"Error code " + std::to_wstring(code);
    }

    if (message != nullptr)
    {
        LocalFree(message);
    }
    return result;
}

bool SameLuid(const LUID& left, const LUID& right)
{
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

bool GetPrimaryGdiName(std::wstring& name, std::wstring& error)
{
    POINT origin{};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
    {
        error = L"Failed to determine primary Windows display: " +
                FormatSystemError(static_cast<LONG>(GetLastError()));
        return false;
    }

    name = info.szDevice;
    return true;
}

bool QueryAdvancedColor(HdrDisplayState& state, std::wstring& error)
{
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 modern{};
    modern.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    modern.header.size = sizeof(modern);
    modern.header.adapterId = state.adapterId;
    modern.header.id = state.targetId;

    LONG status = DisplayConfigGetDeviceInfo(&modern.header);
    if (status == ERROR_SUCCESS)
    {
        state.hdrSupported = modern.highDynamicRangeSupported != 0;
        state.hdrEnabled = modern.highDynamicRangeUserEnabled != 0;
        state.usesModernHdrApi = true;
        return true;
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy{};
    legacy.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    legacy.header.size = sizeof(legacy);
    legacy.header.adapterId = state.adapterId;
    legacy.header.id = state.targetId;

    status = DisplayConfigGetDeviceInfo(&legacy.header);
    if (status == ERROR_SUCCESS)
    {
        state.hdrSupported = legacy.advancedColorSupported != 0;
        state.hdrEnabled = legacy.advancedColorEnabled != 0;
        state.usesModernHdrApi = false;
        return true;
    }

    error = L"Failed to read HDR status: " + FormatSystemError(status);
    return false;
}

bool QueryActivePaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                      std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
                      std::wstring& error)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG status = GetDisplayConfigBufferSizes(
            QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS)
        {
            error = L"Failed enumerate active displays: " + FormatSystemError(status);
            return false;
        }

        paths.assign(pathCount, {});
        modes.assign(modeCount, {});
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                    paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER)
        {
            continue;
        }
        if (status != ERROR_SUCCESS)
        {
            error = L"Failed to query active displays: " + FormatSystemError(status);
            return false;
        }

        paths.resize(pathCount);
        modes.resize(modeCount);
        return true;
    }

    error = L"Display configuration kept changing during the query. Please try again later.";
    return false;
}

bool FillNames(const DISPLAYCONFIG_PATH_INFO& path, HdrDisplayState& state,
               std::wstring& sourceName, std::wstring& error)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;

    LONG status = DisplayConfigGetDeviceInfo(&source.header);
    if (status != ERROR_SUCCESS)
    {
        error = L"Failed to read the display device name: " + FormatSystemError(status);
        return false;
    }
    sourceName = source.viewGdiDeviceName;

    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    status = DisplayConfigGetDeviceInfo(&target.header);
    if (status == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0] != L'\0')
    {
        state.friendlyName = target.monitorFriendlyDeviceName;
    }
    else
    {
        state.friendlyName = sourceName;
    }
    return true;
}

bool QueryByTarget(const LUID& adapterId, UINT32 targetId,
                   HdrDisplayState& state, std::wstring& error)
{
    state.adapterId = adapterId;
    state.targetId = targetId;
    return QueryAdvancedColor(state, error);
}
} // namespace

bool HdrController::QueryPrimaryDisplay(HdrDisplayState& state,
                                        std::wstring& error)
{
    state = {};
    error.clear();

    std::wstring primaryName;
    if (!GetPrimaryGdiName(primaryName, error))
    {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActivePaths(paths, modes, error))
    {
        return false;
    }

    for (const auto& path : paths)
    {
        HdrDisplayState candidate{};
        std::wstring sourceName;
        std::wstring nameError;
        if (!FillNames(path, candidate, sourceName, nameError))
        {
            continue;
        }

        if (_wcsicmp(sourceName.c_str(), primaryName.c_str()) != 0)
        {
            continue;
        }

        candidate.adapterId = path.targetInfo.adapterId;
        candidate.targetId = path.targetInfo.id;
        candidate.gdiDeviceName = sourceName;
        if (!QueryAdvancedColor(candidate, error))
        {
            return false;
        }

        state = std::move(candidate);
        return true;
    }

    error = L"Primary Windows display was not found among the active display paths (" + primaryName + L").";
    return false;
}

bool HdrController::SetEnabled(const HdrDisplayState& display, bool enabled,
                               HdrDisplayState& resultingState,
                               std::wstring& error)
{
    error.clear();
    LONG status = ERROR_NOT_SUPPORTED;

    if (display.usesModernHdrApi)
    {
        DISPLAYCONFIG_SET_HDR_STATE request{};
        request.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
        request.header.size = sizeof(request);
        request.header.adapterId = display.adapterId;
        request.header.id = display.targetId;
        request.enableHdr = enabled ? 1u : 0u;
        status = DisplayConfigSetDeviceInfo(&request.header);
    }

    if (!display.usesModernHdrApi || status == ERROR_NOT_SUPPORTED ||
        status == ERROR_INVALID_PARAMETER || status == ERROR_INVALID_FUNCTION)
    {
        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE request{};
        request.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
        request.header.size = sizeof(request);
        request.header.adapterId = display.adapterId;
        request.header.id = display.targetId;
        request.enableAdvancedColor = enabled ? 1u : 0u;
        status = DisplayConfigSetDeviceInfo(&request.header);
    }

    if (status != ERROR_SUCCESS)
    {
        error = std::wstring(enabled ? L"Enable" : L"Disable") +
                L" HDR Failed：" + FormatSystemError(status);
        resultingState = display;
        return false;
    }

    HdrDisplayState latest = display;
    std::wstring queryError;
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        if (QueryByTarget(display.adapterId, display.targetId, latest, queryError) &&
            latest.hdrEnabled == enabled)
        {
            // Re-query the full primary-display record because a modeset can refresh
            // device names and target metadata.
            HdrDisplayState refreshed{};
            if (QueryPrimaryDisplay(refreshed, queryError))
            {
                resultingState = std::move(refreshed);
            }
            else
            {
                resultingState = latest;
                resultingState.gdiDeviceName = display.gdiDeviceName;
                resultingState.friendlyName = display.friendlyName;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    resultingState = latest;
    error = L"Windows accepted HDR toggle request, but did not report target state before operation timed out.";
    if (!queryError.empty())
    {
        error += L"Last query: " + queryError;
    }
    return false;
}
