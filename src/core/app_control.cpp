#include "core/app_control.h"

#include <windows.h>

namespace rwnd::app_control {

std::optional<uint32_t> LoadStatus(const fs::path& dll) {
    using CreateSection = LONG(NTAPI*)(HANDLE*, ACCESS_MASK, void*, LARGE_INTEGER*, ULONG, ULONG, HANDLE);
    static const auto create = reinterpret_cast<CreateSection>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateSection")));
    if (!create) return std::nullopt;
    HANDLE file =
        CreateFileW(LongPath(dll).c_str(), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    HANDLE section = nullptr;
    const LONG status = create(&section, SECTION_MAP_READ | SECTION_MAP_EXECUTE | SECTION_QUERY, nullptr, nullptr,
                               PAGE_EXECUTE, SEC_IMAGE, file);
    if (status >= 0 && section) CloseHandle(section);
    CloseHandle(file);
    return static_cast<uint32_t>(status);
}

std::optional<uint32_t> SmartAppControlState() {
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy",
                     L"VerifiedAndReputablePolicyState", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::optional<std::string> Refused(const fs::path& root, const StatusOf& status) {
    for (const wchar_t* name : StartupFiles)
        if (status(root / name) == PolicyViolation) return Narrow(name);
    return std::nullopt;
}

std::string Message(std::optional<uint32_t> smartAppControl) {
    return smartAppControl == 1u ? SmartAppControlMessage : PolicyMessage;
}

}
