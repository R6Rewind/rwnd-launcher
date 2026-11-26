#include "core/process.h"

#include <windows.h>
#include <shellapi.h>

namespace rwnd::process {

uint32_t Start(const fs::path& exe, const std::vector<std::wstring>& arguments, const fs::path& workingDirectory) {
    std::wstring command = QuoteArgument(exe.wstring());
    if (!arguments.empty()) command += L" " + JoinArguments(arguments);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring directory = workingDirectory.wstring();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &si, &pi))
        ThrowLastError("Cannot start " + Narrow(exe.filename().wstring()));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}

int RunHidden(const fs::path& exe, const std::vector<std::wstring>& arguments, const fs::path& workingDirectory) {
    std::wstring command = QuoteArgument(exe.wstring());
    if (!arguments.empty()) command += L" " + JoinArguments(arguments);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = si.hStdOutput = si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring directory = workingDirectory.wstring();
    BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             directory.empty() ? nullptr : directory.c_str(), &si, &pi);
    CloseHandle(nul);
    if (!ok) ThrowLastError("Cannot start " + Narrow(exe.filename().wstring()));
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::optional<int> RunElevated(const fs::path& exe, const std::vector<std::wstring>& arguments,
                               const fs::path& workingDirectory, bool wait) {
    std::wstring parameters = JoinArguments(arguments), directory = workingDirectory.wstring(), file = exe.wstring();
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        if (GetLastError() == ERROR_CANCELLED) return std::nullopt;
        ThrowLastError("Windows did not start " + Narrow(exe.filename().wstring()));
    }
    int code = 0;
    if (info.hProcess) {
        if (wait) {
            WaitForSingleObject(info.hProcess, INFINITE);
            DWORD exit = 1;
            GetExitCodeProcess(info.hProcess, &exit);
            code = static_cast<int>(exit);
        }
        CloseHandle(info.hProcess);
    }
    return code;
}

void Open(const std::wstring& target) {
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

fs::path SystemTool(const wchar_t* name) {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    return fs::path(system) / name;
}

}
