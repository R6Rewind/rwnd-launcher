#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/util.h"

namespace rwnd {

struct RememberedAccount {
    Guid playerId;
    std::string displayName;
    std::optional<std::string> avatarUrl;
    std::optional<std::string> refreshToken;
    std::optional<int64_t> refreshExpiresAt;
    bool operator==(const RememberedAccount&) const = default;
};

struct AccountVault {
    std::string apiOrigin;
    std::optional<Guid> selected;
    std::vector<RememberedAccount> accounts;
};

class CredentialStore {
public:
    CredentialStore(fs::path directory, std::string apiOrigin)
        : directory_(std::move(directory)), apiOrigin_(std::move(apiOrigin)) {}
    fs::path FilePath() const { return directory_ / L"accounts.dpapi"; }
    bool WasCorrupt() const { return wasCorrupt_; }
    AccountVault Load();
    void Save(const AccountVault& vault);
    void Delete();
    static void Validate(const AccountVault& vault);
    static std::string Serialize(const AccountVault& vault);
    static AccountVault Deserialize(std::span<const uint8_t> json);

private:
    fs::path directory_;
    std::string apiOrigin_;
    bool wasCorrupt_ = false;
};

class AccountBook {
public:
    static constexpr int Limit = 8;
    explicit AccountBook(const AccountVault& vault);
    const std::vector<RememberedAccount>& Entries() const { return entries_; }
    std::optional<Guid> Selected() const { return selected_; }
    const RememberedAccount* Find(const Guid& id) const;
    void Upsert(const RememberedAccount& account);
    void Select(const Guid& id);
    void Remove(const Guid& id);
    AccountVault Snapshot(const std::string& origin) const { return {origin, selected_, entries_}; }

private:
    std::vector<RememberedAccount> entries_;
    std::optional<Guid> selected_;
};

namespace dpapi {
Bytes Protect(std::span<const uint8_t> plain);
Bytes Unprotect(std::span<const uint8_t> sealed);
void Zero(std::span<uint8_t> bytes);
}

}
