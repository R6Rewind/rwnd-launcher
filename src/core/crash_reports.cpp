#include "core/crash_reports.h"

#include <algorithm>
#include <mutex>

#include "core/instance_folders.h"
#include "core/retention.h"

namespace rwnd::crash_reports {

namespace {
fs::path Record(const fs::path& directory, const Guid& id) {
    return directory / (Widen(id.N()) + L".json");
}
bool BundleAvailable(const fs::path& path) {
    return !path.empty() && path.is_absolute() && path.parent_path().filename() == log_vault::Folder &&
           path.extension() == L".rwndlog" && FileExists(path) && !IsReparsePoint(path);
}
}

Json Pending::ForUi() const {
    const bool log = BundleAvailable(bundles.log);
    return {{"id", id.D()},
            {"displayName", displayName},
            {"exitCode", exitCode ? Json(log_vault::ExitCodeText(*exitCode)) : Json(nullptr)},
            {"hasDump", log && BundleAvailable(bundles.withDump)},
            {"canSend", log},
            {"bundleAvailable", log}};
}

bool UnexpectedExit(std::optional<uint32_t> code, bool hasDump) {
    if (!code) return hasDump;
    return *code == 3 || *code == 0x40000015u || *code == 0x80000003u || (*code & 0xC0000000u) == 0xC0000000u;
}

void Save(const fs::path& directory, const Pending& report) {
    CreateDirectories(directory);
    const auto target = Record(directory, report.id);
    const fs::path temporary = target.wstring() + L".tmp";
    WriteAllText(temporary, ToJson({{"account", report.account.D()},
                                    {"displayName", report.displayName},
                                    {"exitCode", report.exitCode ? Json(*report.exitCode) : Json(nullptr)},
                                    {"log", Narrow(report.bundles.log.wstring())},
                                    {"withDump", Narrow(report.bundles.withDump.wstring())},
                                    {"created", UtcNowTicks()}}));
    MoveReplace(temporary, target);
}

std::optional<Pending> Load(const fs::path& directory, const Guid& id) {
    try {
        const auto path = Record(directory, id);
        if (!FileExists(path) || IsReparsePoint(path)) return std::nullopt;
        const auto json = ParseJson(ReadAllText(path));
        auto account = GetGuid(json, "account");
        if (!account || account->IsEmpty()) return std::nullopt;
        const auto code = GetInt(json, "exitCode");
        const auto created = GetInt(json, "created");
        if (!created || UtcNowTicks() - *created > 30LL * 86400 * TicksPerSecond) {
            DeleteFileIfExists(path);
            return std::nullopt;
        }
        return Pending{id,
                       *account,
                       GetString(json, "displayName").value_or("Parabellum"),
                       code && *code >= 0 && *code <= UINT32_MAX ? std::optional(static_cast<uint32_t>(*code))
                                                                 : std::nullopt,
                       {Widen(GetString(json, "log").value_or("")), Widen(GetString(json, "withDump").value_or(""))}};
    } catch (const Error&) {
        return std::nullopt;
    }
}

std::vector<Pending> PendingReports(const fs::path& directory) {
    std::vector<Pending> result;
    if (!DirExists(directory)) return result;
    auto files = ListFiles(directory, L"*.json");
    std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right) {
        std::error_code leftError, rightError;
        return fs::last_write_time(left, leftError) < fs::last_write_time(right, rightError);
    });
    for (const auto& file : files)
        if (auto id = Guid::Parse(Narrow(file.stem().wstring())))
            if (auto report = Load(directory, *id)) result.push_back(std::move(*report));
    return result;
}

std::vector<Pending> Trim(const fs::path& directory, size_t keep) {
    try {
        auto reports = PendingReports(directory);
        while (reports.size() > keep) {
            try {
                DeleteFileIfExists(Record(directory, reports.front().id));
            } catch (const Error&) {
            }
            reports.erase(reports.begin());
        }
        if (DirExists(directory))
            for (auto& file : ListFiles(directory, L"*.json.tmp")) try {
                    DeleteFileIfExists(file);
                } catch (const Error&) {
                }
        return reports;
    } catch (...) {
        return {};
    }
}

std::set<std::wstring> NeededBundles(const fs::path& directory) {
    std::set<std::wstring> needed;
    try {
        for (auto& report : PendingReports(directory))
            for (auto& bundle : {report.bundles.log, report.bundles.withDump})
                if (!bundle.empty()) needed.insert(retention::Key(bundle));
    } catch (...) {
    }
    return needed;
}

namespace {
std::mutex queueMutex;
fs::path queueOverride;
}

fs::path QueueDirectory() {
    std::lock_guard lock(queueMutex);
    if (!queueOverride.empty()) return queueOverride;
    return fs::path(InstanceFolders::DataDirectory()) / L"PendingCrashReports";
}

void SetQueueDirectory(const fs::path& directory) {
    std::lock_guard lock(queueMutex);
    queueOverride = directory;
}

}
