#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/chat_client.h"
#include "core/http.h"
#include "core/json.h"

namespace rwnd {

struct LoginSession {
    std::string token;
    int64_t expiresAt = 0;
    Guid playerId;
    std::string displayName;
    std::optional<std::string> refreshToken;
    std::optional<int64_t> refreshExpiresAt;
    bool operator==(const LoginSession&) const = default;
    static LoginSession FromJson(const Json& json);
    Json ToJson() const;
};

struct PlayerProfile {
    Guid id;
    std::string displayName;
    std::optional<std::string> avatarUrl;
    std::string role;
    struct Rating {
        std::string season;
        double rating = 0, deviation = 0;
        int64_t wins = 0, losses = 0;
        std::string rank;
    } rating;
    std::optional<int64_t> level, experience;
    std::optional<int64_t> renown, credits;
    static PlayerProfile FromJson(const Json& json);
    Json ToJson() const;
};

enum class SignInProvider { Discord, Steam };
struct SignInProviders {
    bool steam = false;
};

class RememberedLoginApi {
public:
    virtual ~RememberedLoginApi() = default;
    virtual LoginSession Refresh(const std::string& refreshToken, const CancelToken& cancel) = 0;
    virtual void RevokeRefresh(const std::string& refreshToken, const CancelToken& cancel) = 0;
};

class AccountClient : public RememberedLoginApi {
public:
    AccountClient();
    AccountClient(std::string apiBaseUrl, std::shared_ptr<HttpHandler> http,
                  std::function<void(const std::string&)> openBrowser);

    const std::string& ApiBaseUrl() const { return apiBaseUrl_; }
    std::string signInBaseUrl;
    int pickupIntervalMs = 2000;

    LoginSession Login(const CancelToken& cancel, SignInProvider provider = SignInProvider::Discord);
    SignInProviders Providers(const CancelToken& cancel = CancelToken::None());
    LoginSession RedeemCode(const std::string& code, const std::string& codeVerifier, const CancelToken& cancel);
    LoginSession Refresh(const std::string& refreshToken, const CancelToken& cancel) override;
    void RevokeRefresh(const std::string& refreshToken, const CancelToken& cancel) override;
    PlayerProfile Me(const LoginSession& session, const CancelToken& cancel = CancelToken::None());
    static constexpr const char* InviteOnlyMessage =
        "RWND is invite-only right now. Redeem your invite code at rwnd.gg/download to play.";
    bool NeedsInvite(const LoginSession& session, const CancelToken& cancel = CancelToken::None());
    Json News(const CancelToken& cancel);
    Json Announcements(const CancelToken& cancel);
    Json PendingPoll(const LoginSession& session, const CancelToken& cancel);
    Json Presence(const CancelToken& cancel);
    Json Friends(const LoginSession& session, const CancelToken& cancel);
    Json SearchPlayers(const LoginSession& session, const std::string& query, const CancelToken& cancel);
    void FriendAction(const LoginSession& session, const std::string& action, const std::string& username,
                      const CancelToken& cancel);
    std::optional<ChatTicketInfo> ChatTicket(const LoginSession& session, const CancelToken& cancel);
    std::optional<Json> Servers(const LoginSession& session, const CancelToken& cancel);
    Json SetServers(const LoginSession& session, const std::vector<std::string>& allowed, const CancelToken& cancel);
    static Json ServerChoice(const Json& body);
    std::string SubmitCrashReport(const LoginSession& session, std::string bytes, const CancelToken& cancel);
    void RevokeGameSession(const LoginSession& session, const Guid& id, const CancelToken& cancel);
    HttpResponse SignedGet(const std::string& path, const LoginSession& session, const CancelToken& cancel,
                           int timeoutMs = 20000);

    static std::string ConfiguredApiBaseUrl();

private:
    HttpResponse Post(const std::string& path, const Json& body, const CancelToken& cancel, int timeoutMs = 20000);
    HttpResponse Authorized(const std::string& method, const std::string& path, const LoginSession& session,
                            const CancelToken& cancel, std::optional<std::string> body = std::nullopt,
                            std::vector<std::pair<std::string, std::string>> headers = {});
    LoginSession PickupLogin(const std::string& verifier, const std::string& challenge, const CancelToken& cancel);
    LoginSession LoopbackLogin(SignInProvider provider, const std::string& verifier, const std::string& challenge,
                               const CancelToken& cancel);
    Json GetJson(const std::string& path, const CancelToken& cancel);

    std::string apiBaseUrl_;
    std::shared_ptr<HttpHandler> http_;
    std::function<void(const std::string&)> openBrowser_;
};

std::string CallbackPage(const std::string& title, const std::string& body);

}
