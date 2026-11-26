#pragma once

#include <set>

#include "core/log_vault.h"

namespace rwnd::crash_reports {

inline constexpr uint64_t MaxUpload = 16ULL * 1024 * 1024;

struct Pending {
    Guid id;
    Guid account;
    std::string displayName;
    std::optional<uint32_t> exitCode;
    log_vault::CrashBundles bundles;

    Json ForUi() const;
};

bool UnexpectedExit(std::optional<uint32_t> code, bool hasDump);
void Save(const fs::path& directory, const Pending& report);
std::optional<Pending> Load(const fs::path& directory, const Guid& id);
std::vector<Pending> PendingReports(const fs::path& directory);

std::vector<Pending> Trim(const fs::path& directory, size_t keep = 3);
std::set<std::wstring> NeededBundles(const fs::path& directory);
fs::path QueueDirectory();
void SetQueueDirectory(const fs::path& directory);

}
