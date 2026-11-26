#include "core/account_manager.h"

#include <algorithm>

namespace rwnd {

[[noreturn]] void ThrowLoginRequired() {
    Throw(ErrorKind::LoginRequired, "This account needs Discord sign-in. Select it and click Sign in again.");
}

AccountManager::AccountManager(AccountClient& api, CredentialStore& store, RememberedLoginApi* remembered, int limit)
    : api_(api), remembered_(remembered ? *remembered : api), store_(store), book_(store.Load()),
      limit_(std::clamp(limit, 1, AccountBook::Limit)) {}

bool AccountManager::Full(const std::optional<Guid>& incoming) const {
    std::lock_guard lock(bookMutex_);
    if (incoming && book_.Find(*incoming)) return false;
    return static_cast<int>(book_.Entries().size()) >= limit_;
}

std::string AccountManager::LimitMessage() const {
    if (limit_ > 1)
        return "You can add up to " +
               std::string(limit_ == 8   ? "eight"
                           : limit_ == 4 ? "four"
                                         : std::to_string(limit_)) +
               " accounts. Sign out of one first.";
    auto entries = Entries();
    return "RWND Parabellum holds one account per PC. Sign out of " +
           (entries.empty() ? std::string("it") : entries.front().displayName) + " first.";
}

std::vector<RememberedAccount> AccountManager::Entries() const {
    std::lock_guard lock(bookMutex_);
    return book_.Entries();
}
std::optional<Guid> AccountManager::Selected() const {
    std::lock_guard lock(bookMutex_);
    return book_.Selected();
}
std::optional<RememberedAccount> AccountManager::Find(const Guid& id) const {
    std::lock_guard lock(bookMutex_);
    auto a = book_.Find(id);
    return a ? std::optional(*a) : std::nullopt;
}

void AccountManager::Save() {
    store_.Save(book_.Snapshot(api_.ApiBaseUrl()));
}

void AccountManager::Select(const Guid& id) {
    std::lock_guard lock(bookMutex_);
    book_.Select(id);
    Save();
}

void AccountManager::Upsert(const RememberedAccount& account) {
    std::lock_guard lock(bookMutex_);
    book_.Upsert(account);
}

void AccountManager::UpdateIdentity(const PlayerProfile& profile) {
    std::lock_guard lock(bookMutex_);
    auto account = book_.Find(profile.id);
    if (!account || (account->displayName == profile.displayName && account->avatarUrl == profile.avatarUrl) ||
        Trim(profile.displayName).empty())
        return;
    auto selected = book_.Selected();
    RememberedAccount updated = *account;
    updated.displayName = profile.displayName;
    updated.avatarUrl = profile.avatarUrl;
    book_.Upsert(updated);
    if (selected) book_.Select(*selected);
    Save();
}

bool AccountManager::CanSignInSilently(const Guid& id) const {
    std::lock_guard lock(bookMutex_);
    int64_t now = UtcNowTicks();
    if (auto it = sessions_.find(id); it != sessions_.end() && it->second.expiresAt > now + 60 * TicksPerSecond)
        return true;
    auto account = book_.Find(id);
    return account && account->refreshToken && account->refreshExpiresAt && *account->refreshExpiresAt > now;
}

bool AccountManager::Add(const std::optional<Guid>& expected, const CancelToken& cancel, SignInProvider provider) {
    if (Full(expected)) Throw(ErrorKind::InvalidOperation, LimitMessage());
    LoginSession session = api_.Login(cancel, provider);
    auto discard = [&] {
        if (!session.refreshToken) return;
        try {
            remembered_.RevokeRefresh(*session.refreshToken, cancel);
        } catch (const Error& e) {
            if (e.kind != ErrorKind::Http) throw;
        }
    };
    if (expected && session.playerId != *expected) {
        discard();
        if (provider == SignInProvider::Steam)
            Throw(
                ErrorKind::InvalidOperation,
                limit_ > 1
                    ? "Steam returned a different account. Switch Steam accounts in your browser and retry this account, or use Add account."
                    : "Steam returned a different account. Switch Steam accounts in your browser and try again.");
        Throw(
            ErrorKind::InvalidOperation,
            limit_ > 1
                ? "Discord returned a different account. Switch Discord accounts in your browser and retry this account, or use Add account."
                : "Discord returned a different account. Switch Discord accounts in your browser and try again.");
    }
    Keep(session, cancel, true);
    return session.refreshToken.has_value();
}

void AccountManager::Keep(const LoginSession& session, const CancelToken& cancel, bool select) {
    auto discard = [&] {
        if (!session.refreshToken) return;
        try {
            remembered_.RevokeRefresh(*session.refreshToken, cancel);
        } catch (const Error& e) {
            if (e.kind != ErrorKind::Http) throw;
        }
    };
    PlayerProfile profile = api_.Me(session);
    if (profile.id != session.playerId) Throw(ErrorKind::InvalidData, "The login profile does not match the account.");
    if (session.refreshToken && (Trim(*session.refreshToken).empty() || !session.refreshExpiresAt ||
                                 *session.refreshExpiresAt <= UtcNowTicks()))
        Throw(ErrorKind::InvalidData, "The API returned an invalid remembered login credential.");
    std::lock_guard gate(refreshGate_);
    cancel.ThrowIfCanceled();
    if (Full(profile.id)) {
        std::string message = LimitMessage();
        discard();
        Throw(ErrorKind::InvalidOperation, message);
    }
    {
        std::lock_guard lock(bookMutex_);
        auto selected = book_.Selected();
        book_.Upsert(
            {profile.id, profile.displayName, profile.avatarUrl, session.refreshToken, session.refreshExpiresAt});
        if (!select && selected) book_.Select(*selected);
        Save();
        sessions_[session.playerId] = session;
    }
}

Guid AccountManager::AddWithCode(const Guid& expected, const std::string& code, const std::string& codeVerifier,
                                 const CancelToken& cancel) {
    if (Full(expected)) Throw(ErrorKind::InvalidOperation, LimitMessage());
    LoginSession session = api_.RedeemCode(code, codeVerifier, cancel);
    if (!session.refreshToken) Throw(ErrorKind::InvalidData, "The API returned no remembered login.");
    if (session.playerId != expected) {
        try {
            remembered_.RevokeRefresh(*session.refreshToken, cancel);
        } catch (const Error& e) {
            if (e.kind != ErrorKind::Http) throw;
        }
        Throw(ErrorKind::InvalidData, "The API signed in a different account.");
    }
    Keep(session, cancel, false);
    return session.playerId;
}

LoginSession AccountManager::Session(const Guid& id, const CancelToken& cancel) {
    std::lock_guard gate(refreshGate_);
    cancel.ThrowIfCanceled();
    return SessionCore(id, cancel);
}

LoginSession AccountManager::SessionCore(const Guid& id, const CancelToken& cancel) {
    RememberedAccount account;
    {
        std::lock_guard lock(bookMutex_);
        if (auto it = sessions_.find(id);
            it != sessions_.end() && it->second.expiresAt > UtcNowTicks() + 60 * TicksPerSecond)
            return it->second;
        auto found = book_.Find(id);
        if (!found) Throw(ErrorKind::InvalidOperation, "Unknown account.");
        account = *found;
    }
    if (!account.refreshToken) ThrowLoginRequired();
    if (account.refreshExpiresAt.value_or(0) <= UtcNowTicks()) {
        ForgetCredential(account);
        ThrowLoginRequired();
    }
    LoginSession renewed;
    try {
        renewed = remembered_.Refresh(*account.refreshToken, cancel);
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Http && (e.status == 401 || e.status == 403)) {
            ForgetCredential(account);
            ThrowLoginRequired();
        }
        throw;
    }
    int64_t now = UtcNowTicks();
    if (renewed.playerId != id || !renewed.refreshToken || Trim(*renewed.refreshToken).empty() ||
        !renewed.refreshExpiresAt || *renewed.refreshExpiresAt <= now || Trim(renewed.token).empty() ||
        renewed.expiresAt <= now)
        Throw(ErrorKind::InvalidData, "The API returned an invalid remembered login response.");
    std::lock_guard lock(bookMutex_);
    auto selected = book_.Selected();
    RememberedAccount rotated = account;
    if (auto current = book_.Find(id)) rotated = *current;
    rotated.refreshToken = renewed.refreshToken;
    rotated.refreshExpiresAt = renewed.refreshExpiresAt;
    book_.Upsert(rotated);
    if (selected) book_.Select(*selected);
    Save();
    sessions_[id] = renewed;
    return renewed;
}

void AccountManager::Invalidate(const Guid& id) {
    std::lock_guard lock(bookMutex_);
    sessions_.erase(id);
    auto account = book_.Find(id);
    if (!account) Throw(ErrorKind::InvalidOperation, "Unknown account.");
    ForgetCredential(*account);
}

bool AccountManager::Remove(const Guid& id, const CancelToken& cancel) {
    std::optional<std::string> token;
    {
        std::lock_guard gate(refreshGate_);
        std::lock_guard lock(bookMutex_);
        auto account = book_.Find(id);
        if (!account) Throw(ErrorKind::InvalidOperation, "Unknown account.");
        token = account->refreshToken;
        AccountVault remaining{api_.ApiBaseUrl(), std::nullopt, {}};
        for (auto& a : book_.Entries())
            if (a.playerId != id) remaining.accounts.push_back(a);
        auto selected = book_.Selected();
        remaining.selected =
            selected == id
                ? (remaining.accounts.empty() ? std::nullopt : std::optional(remaining.accounts.front().playerId))
                : selected;
        store_.Save(remaining);
        book_.Remove(id);
        sessions_.erase(id);
    }
    return !token || Revoke(*token, cancel);
}

bool AccountManager::Revoke(const std::string& refreshToken, const CancelToken& cancel) {
    try {
        remembered_.RevokeRefresh(refreshToken, cancel);
        return true;
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Http || e.kind == ErrorKind::Canceled || e.kind == ErrorKind::Timeout) return false;
        throw;
    }
}

void AccountManager::SetLimit(int limit) {
    std::lock_guard lock(bookMutex_);
    limit_ = std::clamp(limit, 1, AccountBook::Limit);
}

std::vector<std::string> AccountManager::DropExtras(const std::set<Guid>& keep) {
    std::lock_guard gate(refreshGate_);
    std::lock_guard lock(bookMutex_);
    auto entries = book_.Entries();
    if (static_cast<int>(entries.size()) <= limit_) return {};
    auto selected = book_.Selected();
    std::vector<Guid> order;
    auto push = [&](const Guid& id) {
        if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
    };
    if (selected) push(*selected);
    for (auto& a : entries)
        if (keep.contains(a.playerId)) push(a.playerId);
    size_t wanted = order.size();
    for (auto& a : entries) push(a.playerId);
    size_t kept = std::max(wanted, static_cast<size_t>(limit_));
    std::vector<std::string> tokens;
    for (size_t i = kept; i < order.size(); i++) {
        if (auto a = book_.Find(order[i]); a && a->refreshToken) tokens.push_back(*a->refreshToken);
        book_.Remove(order[i]);
        sessions_.erase(order[i]);
    }
    book_.Select(order.front());
    Save();
    return tokens;
}

void AccountManager::ForgetCredential(const RememberedAccount& account) {
    std::lock_guard lock(bookMutex_);
    auto selected = book_.Selected();
    RememberedAccount forgotten = account;
    if (auto current = book_.Find(account.playerId)) forgotten = *current;
    forgotten.refreshToken.reset();
    forgotten.refreshExpiresAt.reset();
    book_.Upsert(forgotten);
    if (selected) book_.Select(*selected);
    Save();
    sessions_.erase(account.playerId);
}

}
