#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace rwnd::log_vault {

struct ProfileKey {
    std::string keyId;
    Guid playerId;
    Bytes x, y;
    static std::optional<ProfileKey> FromJson(const Json& json);
    Json ToJson() const;
};

const std::vector<std::wstring>& Names();
inline constexpr const wchar_t* Folder = L"RWND-Parabellum-logs";
inline constexpr int Keep = 3;

void SaveKey(const fs::path& root, const ProfileKey& key);
std::optional<ProfileKey> LoadKey(const fs::path& root);

std::optional<fs::path> Seal(const fs::path& root, const std::string& reason,
                             std::optional<uint32_t> exitCode = std::nullopt);
struct CrashBundles {
    fs::path log;
    fs::path withDump;
};
inline constexpr const wchar_t* DumpName = L"rwnd-crash.dmp";
std::optional<CrashBundles> SealCrash(const fs::path& root, std::optional<uint32_t> exitCode);
std::string ExitCodeText(uint32_t code);

Bytes Encrypt(const std::vector<std::pair<std::string, Bytes>>& files, const Json& header, const ProfileKey& key);

}
