#pragma once

#include <chrono>
#include <string>

#include "core/util.h"

namespace rwnd::save_recovery {

inline constexpr int Generations = 3;
inline constexpr int KeepCrashed = 3;
inline constexpr int64_t EarlyExitSeconds = 300;

using Clock = std::chrono::system_clock;

std::string BeforeRun(const fs::path& root, Clock::time_point now);

std::string AfterRun(const fs::path& root, bool abnormalExit, const std::string& routeLog, Clock::time_point now);

bool IsPleaseWaitCrash(const std::string& routeLog);

inline constexpr uint64_t StartupRewardFunction = 0x123ade0, StartupRewardFunctionSize = 0xb9d;
inline constexpr size_t PackPrefix = 13, PackRecordSize = 28, PackSuffix = 23;
inline constexpr uint32_t PackRecordKey = 0x11889E1A;
bool StripPackRecords(Bytes& bytes, int& removed, std::string& reason);

bool SaveLooksValid(const fs::path& file);

}
