#include "nv_digital_vibrance.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <string>

namespace
{
using NvStatus = int;
using NvDisplayHandle = void*;
constexpr NvStatus kNvOk = 0;
constexpr NvStatus kNvEndEnumeration = -7;
constexpr std::uint32_t kNvApiInitializeId = 0x0150E828;
constexpr std::uint32_t kNvApiUnloadId = 0xD22BDD7E;
constexpr std::uint32_t kNvApiGetErrorMessageId = 0x6C2D048C;
constexpr std::uint32_t kNvApiEnumDisplayHandleId = 0x9ABDD40D;
constexpr std::uint32_t kNvApiGetDisplayNameId = 0x22A78B05;
constexpr std::uint32_t kNvApiGetDvcInfoExId = 0x0E45002D;
constexpr std::uint32_t kNvApiSetDvcLevelExId = 0x4A82C2B1;
constexpr std::uint32_t kNvApiStructVersion1 = 1u << 16;

struct NvDvcInfoEx
{
    std::uint32_t version = 0;
    std::int32_t currentLevel = 0;
    std::int32_t minimumLevel = 0;
    std::int32_t maximumLevel = 0;
    std::int32_t defaultLevel = 0;
};
static_assert(sizeof(NvDvcInfoEx) == 20);

using QueryInterfaceFn = void*(__cdecl*)(std::uint32_t);
using InitializeFn = NvStatus(__cdecl*)();
using UnloadFn = NvStatus(__cdecl*)();
using GetErrorMessageFn = NvStatus(__cdecl*)(NvStatus, char*);
using EnumDisplayHandleFn = NvStatus(__cdecl*)(std::uint32_t, NvDisplayHandle*);
using GetDisplayNameFn = NvStatus(__cdecl*)(NvDisplayHandle, char*);
using GetDvcInfoExFn = NvStatus(__cdecl*)(NvDisplayHandle, std::uint32_t,
                                         NvDvcInfoEx*);
using SetDvcLevelExFn = NvStatus(__cdecl*)(NvDisplayHandle, std::uint32_t,
                                          NvDvcInfoEx*);

std::wstring ToWide(const char* text)
{
    if (text == nullptr || *text == '\0')
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (required <= 1)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), required);
    result.pop_back();
    return result;
}

class NvApiSession
{
public:
    NvApiSession() = default;
    NvApiSession(const NvApiSession&) = delete;
    NvApiSession& operator=(const NvApiSession&) = delete;

    ~NvApiSession()
    {
        if (initialized_ && unload_ != nullptr)
        {
            unload_();
        }
        if (module_ != nullptr)
        {
            FreeLibrary(module_);
        }
    }

    bool Open(std::wstring& error)
    {
#if defined(_WIN64)
        module_ = LoadLibraryW(L"nvapi64.dll");
#else
        module_ = LoadLibraryW(L"nvapi.dll");
#endif
        if (module_ == nullptr)
        {
            error = L"没有找到 NVIDIA NVAPI 驱动库。";
            return false;
        }

        queryInterface_ = reinterpret_cast<QueryInterfaceFn>(
            GetProcAddress(module_, "nvapi_QueryInterface"));
        if (queryInterface_ == nullptr)
        {
            error = L"NVIDIA 驱动没有导出 nvapi_QueryInterface。";
            return false;
        }

        initialize_ = Resolve<InitializeFn>(kNvApiInitializeId);
        unload_ = Resolve<UnloadFn>(kNvApiUnloadId);
        getErrorMessage_ = Resolve<GetErrorMessageFn>(kNvApiGetErrorMessageId);
        enumDisplayHandle_ = Resolve<EnumDisplayHandleFn>(kNvApiEnumDisplayHandleId);
        getDisplayName_ = Resolve<GetDisplayNameFn>(kNvApiGetDisplayNameId);
        getDvcInfoEx_ = Resolve<GetDvcInfoExFn>(kNvApiGetDvcInfoExId);
        setDvcLevelEx_ = Resolve<SetDvcLevelExFn>(kNvApiSetDvcLevelExId);

        if (initialize_ == nullptr || enumDisplayHandle_ == nullptr ||
            getDisplayName_ == nullptr || getDvcInfoEx_ == nullptr)
        {
            error = L"当前 NVIDIA 驱动未提供所需的 Digital Vibrance 兼容接口。";
            return false;
        }

        const NvStatus status = initialize_();
        if (status != kNvOk)
        {
            error = L"初始化 NVIDIA NVAPI 失败：" + Describe(status);
            return false;
        }
        initialized_ = true;
        return true;
    }

    bool FindDisplay(const std::wstring& gdiDeviceName,
                     NvDisplayHandle& handle, std::wstring& error)
    {
        for (std::uint32_t index = 0; index < 64; ++index)
        {
            NvDisplayHandle candidate = nullptr;
            const NvStatus enumStatus = enumDisplayHandle_(index, &candidate);
            if (enumStatus == kNvEndEnumeration)
            {
                break;
            }
            if (enumStatus != kNvOk || candidate == nullptr)
            {
                continue;
            }

            std::array<char, 64> name{};
            if (getDisplayName_(candidate, name.data()) != kNvOk)
            {
                continue;
            }

            const std::wstring candidateName = ToWide(name.data());
            if (_wcsicmp(candidateName.c_str(), gdiDeviceName.c_str()) == 0)
            {
                handle = candidate;
                return true;
            }
        }

        error = L"NVIDIA NVAPI 未找到主显示器 " + gdiDeviceName +
                L"。请确认该显示器由 NVIDIA 显卡直接驱动。";
        return false;
    }

    bool Read(NvDisplayHandle display, NvDvcInfoEx& info, std::wstring& error)
    {
        info = {};
        info.version = static_cast<std::uint32_t>(sizeof(info)) |
                       kNvApiStructVersion1;
        const NvStatus status = getDvcInfoEx_(display, 0, &info);
        if (status != kNvOk)
        {
            error = L"读取 NVIDIA Digital Vibrance 失败：" + Describe(status);
            return false;
        }
        return true;
    }

    bool Write(NvDisplayHandle display, NvDvcInfoEx& info,
               std::wstring& error)
    {
        if (setDvcLevelEx_ == nullptr)
        {
            error = L"当前 NVIDIA 驱动只允许读取 Digital Vibrance，未提供设置接口。";
            return false;
        }

        const NvStatus status = setDvcLevelEx_(display, 0, &info);
        if (status != kNvOk)
        {
            error = L"设置 NVIDIA Digital Vibrance 失败：" + Describe(status);
            return false;
        }
        return true;
    }

private:
    template <typename T>
    T Resolve(std::uint32_t id) const
    {
        return queryInterface_ == nullptr
                   ? nullptr
                   : reinterpret_cast<T>(queryInterface_(id));
    }

    std::wstring Describe(NvStatus status) const
    {
        std::array<char, 64> message{};
        if (getErrorMessage_ != nullptr &&
            getErrorMessage_(status, message.data()) == kNvOk &&
            message[0] != '\0')
        {
            return ToWide(message.data()) + L"（" + std::to_wstring(status) + L"）";
        }
        return L"NVAPI 状态 " + std::to_wstring(status);
    }

    HMODULE module_ = nullptr;
    QueryInterfaceFn queryInterface_ = nullptr;
    InitializeFn initialize_ = nullptr;
    UnloadFn unload_ = nullptr;
    GetErrorMessageFn getErrorMessage_ = nullptr;
    EnumDisplayHandleFn enumDisplayHandle_ = nullptr;
    GetDisplayNameFn getDisplayName_ = nullptr;
    GetDvcInfoExFn getDvcInfoEx_ = nullptr;
    SetDvcLevelExFn setDvcLevelEx_ = nullptr;
    bool initialized_ = false;
};

DigitalVibranceState ToPublicState(const NvDvcInfoEx& info)
{
    return {
        info.currentLevel,
        info.minimumLevel,
        info.maximumLevel,
        info.defaultLevel,
    };
}
} // namespace

bool NvDigitalVibrance::Query(const std::wstring& gdiDeviceName,
                              DigitalVibranceState& state,
                              std::wstring& error)
{
    error.clear();
    NvApiSession session;
    if (!session.Open(error))
    {
        return false;
    }

    NvDisplayHandle display = nullptr;
    if (!session.FindDisplay(gdiDeviceName, display, error))
    {
        return false;
    }

    NvDvcInfoEx info{};
    if (!session.Read(display, info, error))
    {
        return false;
    }

    state = ToPublicState(info);
    return true;
}

bool NvDigitalVibrance::Set(const std::wstring& gdiDeviceName, int level,
                            DigitalVibranceState& resultingState,
                            std::wstring& error)
{
    error.clear();
    NvApiSession session;
    if (!session.Open(error))
    {
        return false;
    }

    NvDisplayHandle display = nullptr;
    if (!session.FindDisplay(gdiDeviceName, display, error))
    {
        return false;
    }

    NvDvcInfoEx info{};
    if (!session.Read(display, info, error))
    {
        return false;
    }

    if (level < info.minimumLevel || level > info.maximumLevel)
    {
        error = L"Digital Vibrance 目标值 " + std::to_wstring(level) +
                L" 超出驱动允许范围（" + std::to_wstring(info.minimumLevel) +
                L"–" + std::to_wstring(info.maximumLevel) + L"）。";
        return false;
    }

    info.currentLevel = level;
    if (!session.Write(display, info, error))
    {
        return false;
    }

    NvDvcInfoEx verified{};
    if (!session.Read(display, verified, error))
    {
        return false;
    }
    resultingState = ToPublicState(verified);
    if (verified.currentLevel != level)
    {
        error = L"NVIDIA 驱动接受了设置请求，但读回值为 " +
                std::to_wstring(verified.currentLevel) + L"%，目标值为 " +
                std::to_wstring(level) + L"%。";
        return false;
    }
    return true;
}
