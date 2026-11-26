#pragma once
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/util.h"

namespace rwnd::retention {

inline constexpr size_t Keep = 3;
inline constexpr uint64_t MaxLogBytes = 4ULL * 1024 * 1024;

struct Result {
    int removed = 0;
    uint64_t bytes = 0;
    void Add(const Result& other) {
        removed += other.removed;
        bytes += other.bytes;
    }
};

std::wstring Key(const fs::path& path);

Result KeepNewest(std::vector<fs::path> items, size_t keep, const std::set<std::wstring>& protect = {});

bool CapLog(const fs::path& log, uint64_t max = MaxLogBytes);

bool IsStagingLeftover(const std::wstring& name);

Result TrimBundles(const fs::path& copy, const std::set<std::wstring>& protect);

Result CleanCopy(const fs::path& copy, const std::set<std::wstring>& protect);

Result CleanInstances(const fs::path& instancesRoot, const std::set<std::wstring>& protect,
                      const std::set<std::wstring>& runningCopies);

Result AtStart(const fs::path& dataDirectory, const std::vector<fs::path>& instanceRoots);

std::shared_mutex& PlayLock();

}
