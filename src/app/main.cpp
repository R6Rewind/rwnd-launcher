#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <thread>

#include "app/controller.h"
#include "app/runtime.h"
#include "app/shell.h"
#include "core/installation.h"

using namespace rwnd;

namespace {

std::wstring Sid() {
#ifndef RWND_RELEASE
    if (GetEnvironmentVariableW(L"RWND_DEV_DATA_DIR", nullptr, 0) > 0) return CurrentUserSid() + L".dev";
#endif
    return CurrentUserSid();
}
std::wstring MutexName() {
    return L"Local\\RWND.Parabellum.Launcher." + Sid();
}
std::wstring ShowSignalName() {
    return L"Local\\RWND.Parabellum.Show." + Sid();
}

void Message(const std::wstring& text, const std::wstring& caption = L"RWND Parabellum", UINT flags = MB_OK) {
    MessageBoxW(nullptr, text.c_str(), caption.c_str(), flags);
}

bool Is(const std::vector<std::wstring>& args, std::initializer_list<const wchar_t*> pattern) {
    if (args.size() != pattern.size()) return false;
    size_t i = 0;
    for (auto p : pattern) {
        if (p && args[i] != p) return false;
        i++;
    }
    return true;
}

int RelaunchElevated(const std::vector<std::wstring>& args) {
    SHELLEXECUTEINFOW info{sizeof info};
    std::wstring exe = ExePath().wstring(), parameters = JoinArguments(args), directory = ExeDirectory().wstring();
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return 0;
    if (GetLastError() == ERROR_CANCELLED) {
        if (!Is(args, {L"--tray"}))
            Message(L"RWND Parabellum runs as administrator. Open it again and choose Yes when Windows asks.",
                    L"RWND Parabellum", MB_ICONINFORMATION);
        return 1;
    }
    Message(L"RWND Parabellum could not start as administrator.", L"RWND Parabellum", MB_ICONERROR);
    return 1;
}

int Uninstall(bool owns, bool silent) {
    if (silent) {
        if (!owns) return 1;
        try {
            Installation::Uninstall();
            return 0;
        } catch (const Error&) {
            return 1;
        }
    }
    const wchar_t* caption = L"Uninstall RWND Parabellum";
    if (!owns) {
        Message(L"Close RWND Parabellum before uninstalling it.", caption);
        return 1;
    }
    if (MessageBoxW(
            nullptr,
            L"Uninstall RWND Parabellum?\n\nYour Parabellum game folder is not touched. Saved logins and each account's game copy stay in your local app data so a reinstall picks up where you left off.",
            caption, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return 0;
    try {
        Installation::Uninstall();
    } catch (const Error& e) {
        Message(L"Uninstall did not finish: " + Widen(e.what()), caption, MB_ICONERROR);
        return 1;
    }
    Message(L"RWND Parabellum was uninstalled.", caption);
    return 0;
}

bool IsLauncherFileName(const std::wstring& file) {
    const std::wstring name = ExePath().filename().wstring();
    if (CompareStringOrdinal(file.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    if (file.size() <= name.size() + 5 || CompareStringOrdinal(file.c_str(), static_cast<int>(name.size()) + 1,
                                                               (name + L".").c_str(), -1, TRUE) != CSTR_EQUAL)
        return false;
    std::wstring middle = file.substr(name.size() + 1, file.size() - name.size() - 5);
    return CompareStringOrdinal(file.c_str() + file.size() - 4, 4, L".old", 4, TRUE) == CSTR_EQUAL && !middle.empty() &&
           std::all_of(middle.begin(), middle.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

bool SameLauncherImage(HANDLE process) {
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(process, 0, path, &size)) return false;
    fs::path image(std::wstring(path, size));
    const auto folder = image.parent_path().wstring(), mine = ExeDirectory().wstring();
    return CompareStringOrdinal(folder.c_str(), -1, mine.c_str(), -1, TRUE) == CSTR_EQUAL &&
           IsLauncherFileName(image.filename().wstring());
}

bool StartedBefore(HANDLE process, const FILETIME& mine) {
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &exited, &kernel, &user) && CompareFileTime(&created, &mine) < 0;
}

void EndLeftoverLaunchers() {
    FILETIME mine{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &mine, &exited, &kernel, &user)) return;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{sizeof entry};
    const auto self = GetCurrentProcessId();
    const auto name = ExePath().filename().wstring();
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == self ||
            CompareStringOrdinal(entry.szExeFile, static_cast<int>(name.size()), name.c_str(), -1, TRUE) != CSTR_EQUAL)
            continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                     entry.th32ProcessID);
        if (!process) continue;
        if (SameLauncherImage(process) && StartedBefore(process, mine) &&
            WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            TerminateProcess(process, 1);
        CloseHandle(process);
    }
    CloseHandle(snapshot);
}

void ListenForShowRequests() {
    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, ShowSignalName().c_str());
    if (!signal) return;
    std::thread([signal] {
        while (WaitForSingleObject(signal, INFINITE) == WAIT_OBJECT_0)
            app::PostToUi([] {
                if (auto shell = app::Shell::Instance()) shell->ShowFromTray();
            });
    }).detach();
}

void BringExistingToFront() {
    if (HANDLE signal = OpenEventW(EVENT_MODIFY_STATE, FALSE, ShowSignalName().c_str())) {
        SetEvent(signal);
        CloseHandle(signal);
        return;
    }
    HWND window = FindWindowW(nullptr, L"RWND Parabellum");
    if (!window) {
        Message(L"RWND Parabellum is already open.");
        return;
    }
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

void WriteError(const fs::path& file, const std::string& message) {
    try {
        WriteAllText(file, message);
    } catch (...) {
    }
}

int Run(std::vector<std::wstring> args) {
    if (args.size() >= 2 && args[0] == L"--after") {
        wchar_t* end = nullptr;
        unsigned long previous = wcstoul(args[1].c_str(), &end, 10);
        if (end && *end == 0 && !args[1].empty()) {
            if (HANDLE prior =
                    OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, previous)) {
                if (WaitForSingleObject(prior, 15000) == WAIT_TIMEOUT && SameLauncherImage(prior))
                    TerminateProcess(prior, 1);
                CloseHandle(prior);
            } else if (HANDLE waitOnly = OpenProcess(SYNCHRONIZE, FALSE, previous)) {
                WaitForSingleObject(waitOnly, 15000);
                CloseHandle(waitOnly);
            }
            args.erase(args.begin(), args.begin() + 2);
        }
    }
    if (Is(args, {L"--render-preview", nullptr})) return app::RunPreview(args[1]);
    if (Is(args, {L"--prepare-folder", nullptr, nullptr})) {
        try {
            return Installation::PrepareFolder(args[1], args[2]);
        } catch (...) {
            return 1;
        }
    }
    if ((args.size() == 2 || args.size() == 3) && args[0] == L"--silent-install" && Installation::IsReleaseBuild()) {
        bool shortcuts = !(args.size() == 3 && args[2] == L"--no-shortcuts");
        try {
            Installation::Install(args[1], shortcuts, shortcuts, [](const std::string&) {});
            return 0;
        } catch (const std::exception& e) {
            WriteError(fs::path(TempDirectory()) / L"rwnd-install-error.txt", e.what());
            return 1;
        }
    }
    if (Installation::IsReleaseBuild() && !IsElevated() && !Is(args, {L"--uninstall", L"--silent"}) &&
        !Is(args, {L"--render-preview", nullptr}))
        return RelaunchElevated(args);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, MutexName().c_str());
    bool owns = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
    struct Release {
        HANDLE m;
        bool owns;
        ~Release() {
            if (m) {
                if (owns) ReleaseMutex(m);
                CloseHandle(m);
            }
        }
    } release{mutex, owns};
    bool setup = (Installation::IsReleaseBuild() && !Installation::IsInstalledHere()) || Is(args, {L"--preview-setup"});
    try {
        if (Is(args, {L"--uninstall"}) || Is(args, {L"--uninstall", L"--silent"}))
            return Uninstall(owns, args.size() == 2);
        bool tray = Is(args, {L"--tray"});
        if (!owns && !setup) {
            if (!tray) BringExistingToFront();
            return 0;
        }
        if (owns && !setup) EndLeftoverLaunchers();
        if (Installation::IsReleaseBuild() && Installation::IsInstalledHere() && Installation::RefreshInstalled(tray))
            return 0;
        if (!app::EnsureWebView2Runtime(tray)) return 1;
        app::Controller controller(setup ? app::StartMode::Setup : app::StartMode::Launcher, tray);
        if (!setup) ListenForShowRequests();
        return controller.Run();
    } catch (const std::exception& e) {
        Message(L"RWND Parabellum could not start: " + Widen(e.what()) +
                    L"\n\nCheck access to your Windows local app data folder, then try again.",
                L"RWND Parabellum", MB_ICONERROR);
        return 1;
    }
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#ifdef RWND_RELEASE
    for (auto name :
         {L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", L"WEBVIEW2_USER_DATA_FOLDER",
          L"WEBVIEW2_RELEASE_CHANNEL_PREFERENCE", L"WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER"})
        SetEnvironmentVariableW(name, nullptr);
#endif
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> args;
    for (int i = 1; i < count; i++) args.emplace_back(argv[i]);
    LocalFree(argv);
    int code = Run(std::move(args));
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
