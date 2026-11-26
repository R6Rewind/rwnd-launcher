#include "core/instance_folders.h"

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <thread>

#include "core/json.h"

namespace rwnd {

uint64_t InstancePlan::LinkedBytes() const {
    uint64_t n = 0;
    for (auto& f : files)
        if (f.action == InstanceAction::Hardlink) n += f.length;
    return n;
}
uint64_t InstancePlan::CopiedBytes() const {
    uint64_t n = 0;
    for (auto& f : files)
        if (f.action == InstanceAction::Copy) n += f.length;
    return n;
}

namespace {
std::mutex g_gameFolderMutex;
std::wstring g_gameFolder;
}

void InstanceFolders::SetGameFolder(const std::wstring& gameFolder) {
    std::lock_guard lock(g_gameFolderMutex);
    g_gameFolder = gameFolder;
}

std::wstring InstanceFolders::StorageFor(const std::wstring& dataDirectory, const std::wstring& gameFolder) {
    std::wstring usual = UsualStorageFor(dataDirectory, gameFolder);
    if (TrimW(gameFolder).empty()) return usual;
    fs::path game;
    try {
        game = FullPath(TrimW(gameFolder));
    } catch (...) {
        return usual;
    }
    if (!Within(usual, game.wstring())) return usual;
    std::wstring other = (PathRoot(game) / L"RWND-Data" / L"Parabellum").wstring();
    return Within(other, game.wstring()) ? usual : other;
}

std::wstring InstanceFolders::UsualStorageFor(const std::wstring& dataDirectory, const std::wstring& gameFolder) {
    if (TrimW(gameFolder).empty()) return dataDirectory;
    fs::path gameRoot, dataRoot;
    try {
        gameRoot = PathRoot(FullPath(TrimW(gameFolder)));
        dataRoot = PathRoot(FullPath(dataDirectory));
    } catch (...) {
        return dataDirectory;
    }
    if (gameRoot.empty() || IEquals(gameRoot.wstring(), dataRoot.wstring())) return dataDirectory;
    return (gameRoot / L"RWND" / L"Parabellum").wstring();
}

bool InstanceFolders::MoveStoreOutOfGame(const std::wstring& dataDirectory, const std::wstring& gameFolder) {
    return MoveStore(UsualStorageFor(dataDirectory, gameFolder), StorageFor(dataDirectory, gameFolder));
}

bool InstanceFolders::MoveStore(const fs::path& from, const fs::path& to) {
    if (IEquals(FullPath(from).wstring(), FullPath(to).wstring())) return false;
    fs::path old = from / L"Content", now = to / L"Content";
    if (!DirExists(old) || PathExists(now)) return false;
    RequireNoReparse(old);
    CreateDirectories(to);
    MoveNoReplace(old, now);
    return true;
}

std::wstring InstanceFolders::StorageDirectory() {
    std::wstring game;
    {
        std::lock_guard lock(g_gameFolderMutex);
        game = g_gameFolder;
    }
#ifndef RWND_RELEASE
    if (GetEnvironmentVariableW(L"RWND_DEV_DATA_DIR", nullptr, 0) > 0) return DataDirectory();
#endif
    return StorageFor(DataDirectory(), game);
}

bool InstanceFolders::SupportsHardLinks(const fs::path& anyPathOnDrive) {
    fs::path root;
    try {
        root = PathRoot(FullPath(anyPathOnDrive));
    } catch (...) {
        return false;
    }
    if (root.empty()) return false;
    wchar_t fileSystem[MAX_PATH + 1] = {};
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fileSystem, MAX_PATH + 1))
        return false;
    return IEquals(fileSystem, L"NTFS") || IEquals(fileSystem, L"ReFS");
}

std::wstring InstanceFolders::DataDirectory() {
#ifndef RWND_RELEASE
    static const std::wstring dev = [] {
        wchar_t buffer[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"RWND_DEV_DATA_DIR", buffer, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(buffer, n) : std::wstring();
    }();
    if (!dev.empty()) return dev;
#endif
    return (LocalAppData() / L"RWND" / L"Parabellum").wstring();
}

bool InstanceFolders::Within(const std::wstring& path, const std::wstring& parent) {
    std::wstring p = TrimEndSeparator(path), q = TrimEndSeparator(parent);
    return IEquals(p, q) || IStartsWithW(path, q + L"\\");
}

void InstanceFolders::RequireNoReparse(const fs::path& path) {
    for (fs::path current = FullPath(path);; current = current.parent_path()) {
        if (PathExists(current) && IsReparsePoint(current))
            Throw(ErrorKind::InvalidData, "Game and instance paths cannot contain links or junctions.");
        if (current == current.parent_path()) break;
    }
}

static std::wstring Extended(const fs::path& path) {
    std::wstring p = FullPath(path).wstring();
    if (p.starts_with(L"\\\\?\\")) return p;
    if (p.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

void InstanceFolders::MakeHardLink(const fs::path& newFile, const fs::path& existingFile) {
    if (!::CreateHardLinkW(Extended(newFile).c_str(), Extended(existingFile).c_str(), nullptr))
        Throw(ErrorKind::Win32, "Cannot hardlink game data. Use an NTFS instance folder on the game's volume.",
              static_cast<int>(GetLastError()));
}

static std::wstring Relative(const fs::path& path, const fs::path& base) {
    return path.lexically_relative(base).wstring();
}

InstancePlan InstanceFolders::Plan(const fs::path& sourceIn, const fs::path& destinationIn) {
    fs::path source = FullPath(sourceIn), destination = FullPath(destinationIn);
    if (Within(source.wstring(), destination.wstring()))
        Throw(ErrorKind::InvalidOperation,
              "This game folder is an RWND account copy. Choose the original game folder in Settings.");
    if (Within(destination.wstring(), source.wstring()))
        Throw(ErrorKind::InvalidOperation,
              "RWND can't use this game folder. Move the game into its own folder, then choose it in Settings.");
    if (!IEquals(PathRoot(source).wstring(), PathRoot(destination).wstring()))
        Throw(ErrorKind::InvalidOperation,
              "Instances must be on the same volume as the game for hardlinks. No full-copy fallback is allowed.");
    RequireNoReparse(source);
    RequireNoReparse(destination);
    static const std::set<std::wstring> copyExtensions = {L".exe", L".dll", L".manifest", L".depgraphbin",
                                                          L".bin", L".ini", L".lang"};
    InstancePlan plan{source, destination, {}};
    for (auto& file : ListFiles(source)) {
        RequireNoReparse(file);
        std::wstring name = file.filename().wstring();
        std::wstring ext = ToLowerW(file.extension().wstring());
        if (IEquals(name, L"rwnd_overlay.dll")) {
            plan.files.push_back({name, InstanceAction::Copy, FileLength(file)});
            continue;
        }
        if (IStartsWithW(name, L"rewind") || IStartsWithW(name, L"RWND")) continue;
        if (ext == L".forge")
            plan.files.push_back({name,
                                  IEquals(name, L"datapc64.forge") ? InstanceAction::Copy : InstanceAction::Hardlink,
                                  FileLength(file)});
        else if (copyExtensions.contains(ext))
            plan.files.push_back({name, InstanceAction::Copy, FileLength(file)});
    }
    std::function<void(const fs::path&)> walk = [&](const fs::path& directory) {
        RequireNoReparse(directory);
        for (auto& file : ListFiles(directory)) {
            RequireNoReparse(file);
            plan.files.push_back({Relative(file, source), InstanceAction::Hardlink, FileLength(file)});
        }
        for (auto& child : ListDirectories(directory)) walk(child);
    };
    for (auto folder : {L"sounddata", L"videos"})
        if (DirExists(source / folder)) walk(source / folder);
    return plan;
}

static void DeleteStaging(const fs::path& directory) {
    InstanceFolders::RequireNoReparse(directory);
    for (auto& file : ListFiles(directory)) DeleteFileIfExists(file);
    for (auto& child : ListDirectories(directory)) DeleteStaging(child);
    if (!RemoveDirectoryW(LongPath(directory).c_str())) ThrowLastError("Cannot remove " + Narrow(directory.wstring()));
}

fs::path InstanceFolders::Prepare(const fs::path& source, const fs::path& instancesRoot, const Guid& playerId) {
    port_installer::VerifyExecutable(source);
    fs::path destination = instancesRoot / Widen(playerId.N());
    InstancePlan plan = Plan(source, destination);
    if (DirExists(destination)) {
        fs::path marker = destination / L"instance-source.json";
        bool same = false;
        if (FileExists(marker)) {
            Json recorded = ParseJson(ReadAllText(marker));
            same = recorded.is_string() && recorded.get<std::string>() == Narrow(plan.source.wstring());
        }
        if (!same)
            Throw(ErrorKind::InvalidData,
                  "This account instance belongs to another game installation. Keep the original game path selected.");
        RequirePrivateMutableFiles(destination);
        return destination;
    }
    CreateDirectories(instancesRoot);
    fs::path staging = destination.wstring() + L".preparing-" + Widen(Guid::New().N());
    CreateDirectories(staging);
    try {
        for (auto& item : plan.files) {
            fs::path target = staging / item.relativePath, original = plan.source / item.relativePath;
            CreateDirectories(target.parent_path());
            if (item.action == InstanceAction::Hardlink)
                MakeHardLink(target, original);
            else
                CopyFileTo(original, target, false);
        }
        WriteAllText(staging / L"instance-source.json", ToJson(Json(Narrow(plan.source.wstring()))));
        RequirePrivateMutableFiles(staging);
        port_installer::Install(staging);
        MoveNoReplace(staging, destination);
    } catch (...) {
        if (DirExists(staging)) try {
                DeleteStaging(staging);
            } catch (...) {
            }
        throw;
    }
    return destination;
}

void InstanceFolders::RequirePrivateMutableFiles(const fs::path& directory) {
    RequireNoReparse(directory);
    for (auto& entry : ListEntries(directory)) {
        RequireNoReparse(entry);
        if (DirExists(entry)) {
            std::wstring name = entry.filename().wstring();
            if (name == L"sounddata" || name == L"videos") continue;
            RequirePrivateMutableFiles(entry);
            continue;
        }
        std::wstring name = entry.filename().wstring();
        if (IEquals(entry.extension().wstring(), L".forge") && !IEquals(name, L"datapc64.forge")) continue;
        HANDLE handle =
            CreateFileW(LongPath(entry).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) ThrowLastError("Cannot read " + Narrow(entry.wstring()));
        BY_HANDLE_FILE_INFORMATION info;
        BOOL ok = GetFileInformationByHandle(handle, &info);
        CloseHandle(handle);
        if (!ok) ThrowLastError("Cannot read " + Narrow(entry.wstring()));
        if (info.nNumberOfLinks != 1)
            Throw(ErrorKind::InvalidData,
                  "A mutable instance file is shared through a hardlink. Refusing to update it.");
    }
}

std::string InstanceProcess::ToJson() const {
    Json j = Json::object();
    j["Pid"] = pid;
    j["StartUtcTicks"] = startUtcTicks;
    j["PlayerId"] = playerId.D();
    j["Port"] = port;
    return rwnd::ToJson(j);
}

std::map<Guid, InstanceProcess> GameInstanceGuard::Running(const fs::path& instancesRoot) {
    std::map<Guid, InstanceProcess> result;
    if (!DirExists(instancesRoot)) return result;
    InstanceFolders::RequireNoReparse(instancesRoot);
    std::vector<ProcessInfo> games;
    try {
        games = ProcessesByName(L"RainbowSix.exe", true);
    } catch (const Error&) {
        Throw(ErrorKind::InvalidOperation,
              "Cannot check a running game. Use the same Windows permissions for the launcher and games.");
    }
    for (auto& game : games) {
        if (game.exePath.empty() || !InstanceFolders::Within(game.exePath, FullPath(instancesRoot).wstring())) continue;
        fs::path root = fs::path(game.exePath).parent_path();
        fs::path recordPath = root / L"RWND-Parabellum-instance.pid.json";
        if (!FileExists(recordPath))
            Throw(ErrorKind::InvalidData,
                  "A managed game is still starting or has no instance record. Close it before another launch.");
        Json j = ParseJson(ReadAllText(recordPath));
        Require(j.is_object(), "Invalid instance process record.");
        InstanceProcess record;
        record.pid = static_cast<uint32_t>(GetInt(j, "Pid").value_or(0));
        record.startUtcTicks = GetInt(j, "StartUtcTicks").value_or(0);
        record.playerId = GetGuid(j, "PlayerId").value_or(Guid{});
        record.port = static_cast<int>(GetInt(j, "Port").value_or(0));
        if (record.pid != game.pid || record.startUtcTicks != game.startUtcTicks ||
            root.filename().wstring() != Widen(record.playerId.N()))
            Throw(ErrorKind::InvalidData, "Running instance identity could not be verified.");
        result[record.playerId] = record;
    }
    return result;
}

void GameInstanceGuard::Stop(const InstanceProcess& game) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, game.pid);
    if (!process) return;
    struct Close {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } close{process};
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode) || exitCode != STILL_ACTIVE) return;
    std::wstring path(32768, L'\0');
    DWORD n = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &n) ||
        !IEquals(fs::path(path.substr(0, n)).filename().wstring(), L"RainbowSix.exe"))
        return;
    if (ProcessStartTicks(process).value_or(-1) != game.startUtcTicks) return;
    struct Find {
        DWORD pid;
        HWND found;
    } find{game.pid, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto f = reinterpret_cast<Find*>(lp);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == f->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
                f->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (find.found) PostMessageW(find.found, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process, 8000) == WAIT_OBJECT_0) return;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof entry};
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
            if (entry.th32ParentProcessID == game.pid)
                if (HANDLE child = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID)) {
                    TerminateProcess(child, 1);
                    CloseHandle(child);
                }
        CloseHandle(snapshot);
    }
    TerminateProcess(process, 1);
    WaitForSingleObject(process, INFINITE);
}

std::vector<int> GameInstanceGuard::UdpListeners() {
    std::vector<int> ports;
    for (ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        DWORD size = 0;
        GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_BASIC, 0);
        std::vector<uint8_t> buffer(size + 1024);
        size = static_cast<DWORD>(buffer.size());
        if (GetExtendedUdpTable(buffer.data(), &size, FALSE, family, UDP_TABLE_BASIC, 0) != NO_ERROR) continue;
        if (family == AF_INET) {
            auto table = reinterpret_cast<MIB_UDPTABLE*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++)
                ports.push_back(ntohs(static_cast<u_short>(table->table[i].dwLocalPort)));
        } else {
            auto table = reinterpret_cast<MIB_UDP6TABLE*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++)
                ports.push_back(ntohs(static_cast<u_short>(table->table[i].dwLocalPort)));
        }
    }
    return ports;
}

int GameInstanceGuard::CheckAndChoosePort(const fs::path& instancesRoot, const Guid& playerId,
                                          const std::optional<fs::path>& original) {
    if (original) {
        std::vector<ProcessInfo> games;
        try {
            games = ProcessesByName(L"RainbowSix.exe", true);
        } catch (const Error&) {
            Throw(ErrorKind::InvalidOperation,
                  "Cannot check a running game. Use the same Windows permissions for the launcher and games.");
        }
        for (auto& g : games)
            if (IEquals(g.exePath, (*original / L"RainbowSix.exe").wstring()))
                Throw(ErrorKind::InvalidOperation,
                      "Close the game launched from the original folder before using account instances.");
    }
    return ChoosePort(Running(instancesRoot), playerId, UdpListeners());
}

int GameInstanceGuard::ChoosePort(const std::map<Guid, InstanceProcess>& running, const Guid& playerId,
                                  const std::vector<int>& listeners) {
    if (running.contains(playerId)) Throw(ErrorKind::InvalidOperation, "This account already has a running game.");
    if (running.size() >= GamePorts.size())
        Throw(ErrorKind::InvalidOperation, "Eight game sessions are already running.");
    std::set<int> used(listeners.begin(), listeners.end());
    for (auto& [id, game] : running) used.insert(game.port);
    for (int port : GamePorts)
        if (!used.contains(port)) return port;
    Throw(ErrorKind::InvalidOperation,
          "The eight game UDP ports are busy. Close an existing session before launching.");
}

}
