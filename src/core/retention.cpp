#include "core/retention.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string_view>

#include "core/crash_reports.h"
#include "core/instance_folders.h"
#include "core/log_vault.h"

namespace rwnd::retention {

namespace {

bool Hex32(std::wstring_view text) {
    return text.size() == 32 && std::all_of(text.begin(), text.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; });
}

bool EndsWith(std::wstring_view text, std::wstring_view end) {
    return text.size() >= end.size() && IEquals(text.substr(text.size() - end.size()), end);
}

bool Reparse(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(LongPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

fs::file_time_type WriteTime(const fs::path& path) {
    std::error_code ec;
    auto time = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : time;
}

bool Remove(const fs::path& path, Result& result) {
    const std::wstring longPath = LongPath(path);
    const DWORD attributes = GetFileAttributesW(longPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(longPath.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        uint64_t length = 0;
        try {
            length = FileLength(path);
        } catch (...) {
        }
        if (!DeleteFileW(longPath.c_str())) return false;
        result.removed++;
        result.bytes += length;
        return true;
    }
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::vector<fs::path> children;
        try {
            children = ListEntries(path);
        } catch (...) {
            return false;
        }
        for (auto& child : children) Remove(child, result);
    }
    if (!RemoveDirectoryW(longPath.c_str())) return false;
    result.removed++;
    return true;
}

void RemoveLeftovers(const fs::path& directory, Result& result, int depth) {
    if (depth > 8 || Reparse(directory)) return;
    std::vector<fs::path> entries;
    try {
        entries = ListEntries(directory);
    } catch (...) {
        return;
    }
    for (auto& entry : entries) {
        const std::wstring name = entry.filename().wstring();
        if (DirExists(entry)) {
            if (!IEquals(name, log_vault::Folder)) RemoveLeftovers(entry, result, depth + 1);
            continue;
        }
        if (IsStagingLeftover(name)) Remove(entry, result);
    }
}

}

std::wstring Key(const fs::path& path) {
    try {
        return ToLowerW(FullPath(path).lexically_normal().wstring());
    } catch (...) {
        return ToLowerW(path.lexically_normal().wstring());
    }
}

Result KeepNewest(std::vector<fs::path> items, size_t keep, const std::set<std::wstring>& protect) {
    Result result;
    try {
        std::vector<std::pair<fs::file_time_type, fs::path>> candidates;
        for (auto& item : items)
            if (!protect.contains(Key(item))) candidates.emplace_back(WriteTime(item), item);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return ToLowerW(a.second.filename().wstring()) > ToLowerW(b.second.filename().wstring());
        });
        for (size_t i = keep; i < candidates.size(); i++) Remove(candidates[i].second, result);
    } catch (...) {
    }
    return result;
}

bool CapLog(const fs::path& log, uint64_t max) {
    if (max < 2) return false;
    HANDLE file = CreateFileW(LongPath(log).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool capped = false;
    try {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) > max && !Reparse(log)) {
            const uint64_t length = static_cast<uint64_t>(size.QuadPart), keep = max / 2;
            std::string tail(static_cast<size_t>(keep), '\0');
            LARGE_INTEGER at{};
            at.QuadPart = static_cast<LONGLONG>(length - keep);
            DWORD read = 0;
            if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN) &&
                ReadFile(file, tail.data(), static_cast<DWORD>(keep), &read, nullptr) && read == keep) {
                if (auto line = tail.find('\n'); line != std::string::npos && line + 1 < tail.size())
                    tail.erase(0, line + 1);
                const std::string note = "[RWND: this log passed " + std::to_string(max / (1024 * 1024)) +
                                         " MB; older lines were removed]\r\n";
                std::string text = note + tail;
                LARGE_INTEGER start{};
                DWORD written = 0;
                capped = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) &&
                         WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                         written == text.size() && SetEndOfFile(file);
            }
        }
    } catch (...) {
        capped = false;
    }
    CloseHandle(file);
    return capped;
}

bool IsStagingLeftover(const std::wstring& name) {
    if (const auto at = ToLowerW(name).rfind(L".rwnd-"); at != std::wstring::npos && at > 0) {
        const std::wstring_view rest = std::wstring_view(name).substr(at + 6);
        if (const auto dash = rest.rfind(L'-');
            dash != std::wstring_view::npos && dash > 0 && Hex32(rest.substr(dash + 1))) {
            const auto step = rest.substr(0, dash);
            if (std::all_of(step.begin(), step.end(), [](wchar_t c) {
                    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-';
                }))
                return true;
        }
        if (IEquals(rest, L"tmp")) return true;
    }
    if (const auto at = ToLowerW(name).rfind(L".tmp-");
        at != std::wstring::npos && at > 0 && Hex32(std::wstring_view(name).substr(at + 5)))
        return true;
    if (EndsWith(name, L".rwndlog.tmp")) return name.size() > 12;
    const std::wstring dump = std::wstring(log_vault::DumpName) + L".";
    if (name.size() > dump.size() + 8 && IEquals(std::wstring_view(name).substr(0, dump.size()), dump) &&
        EndsWith(name, L".pending")) {
        const auto pid = std::wstring_view(name).substr(dump.size(), name.size() - dump.size() - 8);
        return !pid.empty() && std::all_of(pid.begin(), pid.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    }
    return false;
}

Result TrimBundles(const fs::path& copy, const std::set<std::wstring>& protect) {
    Result result;
    try {
        const fs::path folder = copy / log_vault::Folder;
        if (!DirExists(folder) || Reparse(folder)) return result;
        std::vector<fs::path> dumps, logs;
        for (auto& file : ListFiles(folder, L"*.rwndlog"))
            (EndsWith(file.filename().wstring(), L"-with-dump.rwndlog") ? dumps : logs).push_back(file);
        result.Add(KeepNewest(dumps, Keep, protect));
        result.Add(KeepNewest(logs, Keep, protect));
        for (auto& file : ListFiles(folder, L"*.rwndlog.tmp")) Remove(file, result);
    } catch (...) {
    }
    return result;
}

Result CleanCopy(const fs::path& copy, const std::set<std::wstring>& protect) {
    Result result;
    try {
        if (!DirExists(copy) || Reparse(copy)) return result;
        result.Add(TrimBundles(copy, protect));
        const fs::path backups = copy / L"RWND-Parabellum-backups";
        if (DirExists(backups) && !Reparse(backups)) result.Add(KeepNewest(ListDirectories(backups), Keep));
        RemoveLeftovers(copy, result, 0);
        for (auto& name : log_vault::Names()) {
            const fs::path log = copy / name;
            if (FileExists(log) && CapLog(log)) result.removed++;
        }
    } catch (...) {
    }
    return result;
}

Result CleanInstances(const fs::path& instancesRoot, const std::set<std::wstring>& protect,
                      const std::set<std::wstring>& runningCopies) {
    Result result;
    try {
        if (!DirExists(instancesRoot) || Reparse(instancesRoot)) return result;
        for (auto& folder : ListDirectories(instancesRoot)) {
            const std::wstring name = folder.filename().wstring();
            if (Hex32(name)) {
                if (runningCopies.contains(ToLowerW(name)))
                    result.Add(TrimBundles(folder, protect));
                else
                    result.Add(CleanCopy(folder, protect));
                continue;
            }
            constexpr std::wstring_view marker = L".preparing-";
            if (name.size() == 32 + marker.size() + 32 && Hex32(std::wstring_view(name).substr(0, 32)) &&
                IEquals(std::wstring_view(name).substr(32, marker.size()), marker) &&
                Hex32(std::wstring_view(name).substr(32 + marker.size())))
                Remove(folder, result);
        }
    } catch (...) {
    }
    return result;
}

std::shared_mutex& PlayLock() {
    static std::shared_mutex lock;
    return lock;
}

Result AtStart(const fs::path& dataDirectory, const std::vector<fs::path>& instanceRoots) {
    Result result;
    std::unique_lock exclusive(PlayLock());
    try {
        crash_reports::Trim(crash_reports::QueueDirectory(), Keep);
        const auto needed = crash_reports::NeededBundles(crash_reports::QueueDirectory());
        std::set<std::wstring> done;
        for (auto& root : instanceRoots) {
            if (root.empty() || !done.insert(Key(root)).second) continue;
            std::set<std::wstring> running;
            try {
                for (auto& [id, game] : GameInstanceGuard::Running(root)) running.insert(ToLowerW(Widen(id.N())));
            } catch (...) {
                continue;
            }
            result.Add(CleanInstances(root, needed, running));
        }
        const fs::path helpers = dataDirectory / L"CrashHelper";
        if (DirExists(helpers) && !Reparse(helpers)) result.Add(KeepNewest(ListDirectories(helpers), Keep));
    } catch (...) {
    }
    return result;
}

}
