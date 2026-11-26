#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <set>

#include "core/account_client.h"
#include "core/credential_store.h"

namespace rwnd {

class AccountManager {
public:
    AccountManager(AccountClient& api, CredentialStore& store, RememberedLoginApi* remembered = nullptr,
                   int limit = AccountBook::Limit);

    int Limit() const { return limit_.load(); }
    void SetLimit(int limit);
    std::vector<RememberedAccount> Entries() const;
    std::optional<Guid> Selected() const;
    std::optional<RememberedAccount> Find(const Guid& id) const;
    bool WasCorrupt() const { return store_.WasCorrupt(); }

    void Select(const Guid& id);
    void UpdateIdentity(const PlayerProfile& profile);
    bool CanSignInSilently(const Guid& id) const;
    bool Add(const std::optional<Guid>& expected, const CancelToken& cancel,
             SignInProvider provider = SignInProvider::Discord);
    Guid AddWithCode(const Guid& expected, const std::string& code, const std::string& codeVerifier,
                     const CancelToken& cancel);
    LoginSession Session(const Guid& id, const CancelToken& cancel);
    void Invalidate(const Guid& id);
    bool Remove(const Guid& id, const CancelToken& cancel);
    std::vector<std::string> DropExtras(const std::set<Guid>& keep);
    bool Revoke(const std::string& refreshToken, const CancelToken& cancel);

    void Upsert(const RememberedAccount& account);

private:
    LoginSession SessionCore(const Guid& id, const CancelToken& cancel);
    void Keep(const LoginSession& session, const CancelToken& cancel, bool select);
    void ForgetCredential(const RememberedAccount& account);
    void Save();
    bool Full(const std::optional<Guid>& incoming) const;
    std::string LimitMessage() const;

    AccountClient& api_;
    RememberedLoginApi& remembered_;
    CredentialStore& store_;
    AccountBook book_;
    mutable std::recursive_mutex bookMutex_;
    std::mutex refreshGate_;
    std::map<Guid, LoginSession> sessions_;
    std::atomic<int> limit_;
};

[[noreturn]] void ThrowLoginRequired();

}
