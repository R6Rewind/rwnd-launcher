#include "core/credential_store.h"

#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <set>

#include "core/json.h"

namespace rwnd {

namespace dpapi {
void Zero(std::span<uint8_t> bytes) {
    SecureZeroMemory(bytes.data(), bytes.size());
}

static Bytes Transform(std::span<const uint8_t> value, bool protect) {
    Bytes copy(value.begin(), value.end());
    DATA_BLOB input{static_cast<DWORD>(copy.size()), copy.data()}, output{};
    BOOL ok = protect
                  ? CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
                  : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    Zero(copy);
    if (!ok) Throw(ErrorKind::InvalidData, "Windows could not protect or read saved login data.");
    Bytes result(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
Bytes Protect(std::span<const uint8_t> plain) {
    return Transform(plain, true);
}
Bytes Unprotect(std::span<const uint8_t> sealed) {
    return Transform(sealed, false);
}
}

void CredentialStore::Validate(const AccountVault& vault) {
    bool bad = static_cast<int>(vault.accounts.size()) > AccountBook::Limit;
    std::set<Guid> ids;
    for (auto& a : vault.accounts) {
        if (a.playerId.IsEmpty() || Trim(a.displayName).empty()) bad = true;
        ids.insert(a.playerId);
    }
    if (ids.size() != vault.accounts.size()) bad = true;
    if (vault.selected && !ids.contains(*vault.selected)) bad = true;
    if (bad) Throw(ErrorKind::InvalidData, "Invalid saved account list.");
}

static Json Optional(const std::optional<std::string>& v) {
    return v ? Json(*v) : Json(nullptr);
}

std::string CredentialStore::Serialize(const AccountVault& vault) {
    Json accounts = Json::array();
    for (auto& a : vault.accounts) {
        Json j = Json::object();
        j["PlayerId"] = a.playerId.D();
        j["DisplayName"] = a.displayName;
        j["AvatarUrl"] = Optional(a.avatarUrl);
        j["RefreshToken"] = Optional(a.refreshToken);
        j["RefreshExpiresAt"] = a.refreshExpiresAt ? Json(FormatIsoTicks(*a.refreshExpiresAt)) : Json(nullptr);
        accounts.push_back(std::move(j));
    }
    Json root = Json::object();
    root["ApiOrigin"] = vault.apiOrigin;
    root["Selected"] = vault.selected ? Json(vault.selected->D()) : Json(nullptr);
    root["Accounts"] = std::move(accounts);
    return ToJson(root);
}

AccountVault CredentialStore::Deserialize(std::span<const uint8_t> bytes) {
    Json root = ParseJson(bytes);
    Require(root.is_object(), "Invalid saved account list.");
    AccountVault vault;
    vault.apiOrigin = GetString(root, "ApiOrigin").value_or("");
    vault.selected = GetGuid(root, "Selected");
    const Json* list = Field(root, "Accounts");
    Require(list && list->is_array(), "Invalid saved account list.");
    for (auto& item : *list) {
        Require(item.is_object(), "Invalid saved account list.");
        RememberedAccount a;
        a.playerId = GetGuid(item, "PlayerId").value_or(Guid{});
        a.displayName = GetString(item, "DisplayName").value_or("");
        a.avatarUrl = GetString(item, "AvatarUrl");
        a.refreshToken = GetString(item, "RefreshToken");
        a.refreshExpiresAt = GetTicks(item, "RefreshExpiresAt");
        vault.accounts.push_back(std::move(a));
    }
    return vault;
}

AccountVault CredentialStore::Load() {
    wasCorrupt_ = false;
    if (!FileExists(FilePath())) return {apiOrigin_, std::nullopt, {}};
    Bytes plain;
    try {
        if (FileLength(FilePath()) > 128 * 1024) Throw(ErrorKind::InvalidData, "Saved login data is too large.");
        plain = dpapi::Unprotect(ReadAllBytes(FilePath()));
        AccountVault vault = Deserialize(plain);
        dpapi::Zero(plain);
        Validate(vault);
        if (vault.apiOrigin != apiOrigin_) Throw(ErrorKind::InvalidData, "Saved logins belong to another server.");
        return vault;
    } catch (const Error& e) {
        dpapi::Zero(plain);
        if (e.kind != ErrorKind::InvalidData) throw;
        wasCorrupt_ = true;
        DeleteFileIfExists(FilePath());
        return {apiOrigin_, std::nullopt, {}};
    }
}

void CredentialStore::Save(const AccountVault& vault) {
    Validate(vault);
    if (vault.apiOrigin != apiOrigin_) Throw(ErrorKind::InvalidData, "Wrong credential origin.");
    if (vault.accounts.empty()) {
        Delete();
        return;
    }
    CreateDirectories(directory_);
    std::string plain = Serialize(vault);
    fs::path temporary = FilePath().wstring() + L"." + Widen(Guid::New().N()) + L".tmp";
    try {
        Bytes sealed = dpapi::Protect(std::span(reinterpret_cast<const uint8_t*>(plain.data()), plain.size()));
        SecureZeroMemory(plain.data(), plain.size());
        WriteAllBytes(temporary, sealed);
        MoveReplace(temporary, FilePath());
    } catch (...) {
        SecureZeroMemory(plain.data(), plain.size());
        try {
            DeleteFileIfExists(temporary);
        } catch (...) {
        }
        throw;
    }
}

void CredentialStore::Delete() {
    DeleteFileIfExists(FilePath());
}

AccountBook::AccountBook(const AccountVault& vault) {
    CredentialStore::Validate(vault);
    entries_ = vault.accounts;
    selected_ =
        vault.selected ? vault.selected : (entries_.empty() ? std::nullopt : std::optional(entries_.front().playerId));
}

const RememberedAccount* AccountBook::Find(const Guid& id) const {
    for (auto& a : entries_)
        if (a.playerId == id) return &a;
    return nullptr;
}

void AccountBook::Upsert(const RememberedAccount& account) {
    if (account.playerId.IsEmpty() || Trim(account.displayName).empty())
        Throw(ErrorKind::InvalidData, "Invalid account identity.");
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](auto& a) { return a.playerId == account.playerId; });
    if (it != entries_.end())
        *it = account;
    else {
        if (static_cast<int>(entries_.size()) >= Limit)
            Throw(ErrorKind::InvalidOperation, "You can add up to eight accounts. Sign out of one first.");
        entries_.push_back(account);
    }
    selected_ = account.playerId;
}

void AccountBook::Select(const Guid& id) {
    if (!Find(id)) Throw(ErrorKind::InvalidOperation, "Unknown account.");
    selected_ = id;
}

void AccountBook::Remove(const Guid& id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](auto& a) { return a.playerId == id; }),
                   entries_.end());
    if (selected_ == id) selected_ = entries_.empty() ? std::nullopt : std::optional(entries_.front().playerId);
}

}
