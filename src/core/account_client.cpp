#include "core/account_client.h"

#include <algorithm>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include "core/settings.h"

namespace rwnd {

LoginSession LoginSession::FromJson(const Json& j) {
    Require(j.is_object(), "Empty login response.");
    LoginSession s;
    s.token = GetString(j, "token").value_or("");
    s.expiresAt = GetTicks(j, "expiresAt").value_or(0);
    s.playerId = GetGuid(j, "playerId").value_or(Guid{});
    s.displayName = GetString(j, "displayName").value_or("");
    s.refreshToken = GetString(j, "refreshToken");
    s.refreshExpiresAt = GetTicks(j, "refreshExpiresAt");
    return s;
}

Json LoginSession::ToJson() const {
    Json j = Json::object();
    j["token"] = token;
    j["expiresAt"] = FormatIsoTicks(expiresAt);
    j["playerId"] = playerId.D();
    j["displayName"] = displayName;
    j["refreshToken"] = refreshToken ? Json(*refreshToken) : Json(nullptr);
    j["refreshExpiresAt"] = refreshExpiresAt ? Json(FormatIsoTicks(*refreshExpiresAt)) : Json(nullptr);
    return j;
}

PlayerProfile PlayerProfile::FromJson(const Json& j) {
    Require(j.is_object(), "Empty player profile.");
    PlayerProfile p;
    p.id = GetGuid(j, "id").value_or(Guid{});
    p.displayName = GetString(j, "displayName").value_or("");
    p.avatarUrl = GetString(j, "avatarUrl");
    p.role = GetString(j, "role").value_or("");
    if (const Json* r = Field(j, "rating"); r && r->is_object()) {
        p.rating.season = GetString(*r, "season").value_or("");
        p.rating.rating = GetDouble(*r, "rating").value_or(0);
        p.rating.deviation = GetDouble(*r, "deviation").value_or(0);
        p.rating.wins = GetInt(*r, "wins").value_or(0);
        p.rating.losses = GetInt(*r, "losses").value_or(0);
        p.rating.rank = GetString(*r, "rank").value_or("");
    }
    if (const Json* g = Field(j, "progression"); g && g->is_object()) {
        p.level = GetInt(*g, "level").value_or(0);
        p.experience = GetInt(*g, "experience").value_or(0);
    }
    if (const Json* w = Field(j, "wallet"); w && w->is_object()) {
        p.renown = GetInt(*w, "renown").value_or(0);
        p.credits = GetInt(*w, "credits").value_or(0);
    }
    return p;
}

Json PlayerProfile::ToJson() const {
    Json j = Json::object();
    j["id"] = id.D();
    j["displayName"] = displayName;
    j["avatarUrl"] = avatarUrl ? Json(*avatarUrl) : Json(nullptr);
    j["role"] = role;
    j["rating"] = {{"season", rating.season}, {"rating", rating.rating}, {"deviation", rating.deviation},
                   {"wins", rating.wins},     {"losses", rating.losses}, {"rank", rating.rank}};
    j["progression"] = level ? Json{{"level", *level}, {"experience", experience.value_or(0)}} : Json(nullptr);
    j["wallet"] = renown ? Json{{"renown", *renown}, {"credits", credits.value_or(0)}} : Json(nullptr);
    return j;
}

std::string AccountClient::ConfiguredApiBaseUrl() {
    std::string configured = settings::Endpoint(L"R6REWIND_API_URL")
                                 .value_or(settings::String("apiBaseUrl").value_or("https://rwnd.gg/api"));
    while (!configured.empty() && configured.back() == '/') configured.pop_back();
    auto url = Url::Parse(configured + "/");
    if (!url || !(url->IsHttps() || (url->scheme == "http" && url->IsLoopback())))
        Throw(ErrorKind::InvalidOperation, "The API address must use HTTPS, or HTTP on localhost for development.");
    return url->Origin() + url->pathAndQuery;
}

static void OpenInBrowser(const std::string& url) {
    ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

AccountClient::AccountClient() : AccountClient(ConfiguredApiBaseUrl(), WinHttp(), OpenInBrowser) {
    if (!settings::Endpoint(L"R6REWIND_API_URL")) {
        std::string site = settings::String("signInBaseUrl").value_or("");
        while (!site.empty() && site.back() == '/') site.pop_back();
        if (auto url = Url::Parse(site + "/"); url && url->IsHttps()) signInBaseUrl = url->Origin() + url->pathAndQuery;
    }
}

AccountClient::AccountClient(std::string apiBaseUrl, std::shared_ptr<HttpHandler> http,
                             std::function<void(const std::string&)> openBrowser)
    : signInBaseUrl(apiBaseUrl), apiBaseUrl_(std::move(apiBaseUrl)), http_(std::move(http)),
      openBrowser_(std::move(openBrowser)) {}

HttpResponse AccountClient::Post(const std::string& path, const Json& body, const CancelToken& cancel, int timeoutMs) {
    HttpRequest request;
    request.method = "POST";
    request.url = ResolveUrl(apiBaseUrl_, path);
    request.body = ToJson(body);
    request.timeoutMs = timeoutMs;
    return http_->Send(request, cancel);
}

HttpResponse AccountClient::Authorized(const std::string& method, const std::string& path, const LoginSession& session,
                                       const CancelToken& cancel, std::optional<std::string> body,
                                       std::vector<std::pair<std::string, std::string>> headers) {
    HttpRequest request;
    request.method = method;
    request.url = ResolveUrl(apiBaseUrl_, path);
    request.headers = std::move(headers);
    request.headers.insert(request.headers.begin(), {"Authorization", "Bearer " + session.token});
    request.body = std::move(body);
    return http_->Send(request, cancel);
}

HttpResponse AccountClient::SignedGet(const std::string& path, const LoginSession& session, const CancelToken& cancel,
                                      int timeoutMs) {
    HttpRequest request;
    request.url = ResolveUrl(apiBaseUrl_, path);
    request.headers.push_back({"Authorization", "Bearer " + session.token});
    request.timeoutMs = timeoutMs;
    return http_->Send(request, cancel);
}

Json AccountClient::GetJson(const std::string& path, const CancelToken& cancel) {
    HttpRequest request;
    request.url = ResolveUrl(apiBaseUrl_, path);
    HttpResponse response = http_->Send(request, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
    return ParseJson(response.body);
}

LoginSession AccountClient::Login(const CancelToken& cancel, SignInProvider provider) {
    std::string verifier = Base64Url(RandomBytes(32));
    std::string challenge =
        Base64Url(Sha256Of(std::span(reinterpret_cast<const uint8_t*>(verifier.data()), verifier.size())));
    CancelSource timeout(cancel);
    timeout.CancelAfter(5 * 60 * 1000);
    bool pickup = false;
    if (provider == SignInProvider::Discord) {
        HttpResponse probe = Post("auth/launcher/pickup", {{"codeVerifier", verifier}}, timeout.Token());
        pickup = probe.status == 410;
    }
    try {
        return pickup ? PickupLogin(verifier, challenge, timeout.Token())
                      : LoopbackLogin(provider, verifier, challenge, timeout.Token());
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Canceled || (e.kind == ErrorKind::Timeout && timeout.Token().IsCanceled()))
            Throw(ErrorKind::Canceled, "The operation was canceled.");
        throw;
    }
}

LoginSession AccountClient::PickupLogin(const std::string& verifier, const std::string& challenge,
                                        const CancelToken& cancel) {
    openBrowser_(ResolveUrl(signInBaseUrl, "auth/discord/login?mode=launcher&codeChallenge=" + challenge));
    for (;;) {
        if (!cancel.Sleep(pickupIntervalMs)) cancel.ThrowIfCanceled();
        cancel.ThrowIfCanceled();
        HttpResponse response;
        try {
            response = Post("auth/launcher/pickup", {{"codeVerifier", verifier}, {"rememberMe", true}}, cancel);
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Http || (e.kind == ErrorKind::Timeout && !cancel.IsCanceled())) continue;
            throw;
        }
        int status = response.status;
        if (status == 200) return LoginSession::FromJson(ParseJson(response.body));
        if (status == 202 || status == 429 || status >= 500) continue;
        if (status == 409 || status == 403 || status == 410) {
            std::optional<std::string> state, error;
            try {
                Json body = ParseJson(response.body);
                if (body.is_object()) {
                    if (const Json* s = Field(body, "status"); s && s->is_string()) state = s->get<std::string>();
                    if (const Json* e = Field(body, "error"); e && e->is_string()) error = e->get<std::string>();
                }
            } catch (const Error&) {
            }
            std::string message = state == "cancelled"    ? "Discord sign-in was cancelled. Try again."
                                  : state == "restricted" ? error.value_or("This account or connection is restricted.")
                                  : state == "expired"    ? "Sign-in expired. Try again."
                                                          : "Discord sign-in failed. Try again.";
            Throw(ErrorKind::InvalidOperation, message);
        }
        if (status < 200 || status > 299) ThrowStatus(status);
        Throw(ErrorKind::InvalidData, "Unexpected sign-in answer " + std::to_string(status) + ".");
    }
}

std::string CallbackPage(const std::string& title, const std::string& body) {
    return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>RWND Parabellum</title><style>"
           "body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0E1013;color:#E8E4DA;font:500 17px 'Segoe UI',sans-serif}"
           "main{border:1px solid #2A2E35;background:#171A1F;border-radius:8px;padding:40px 48px;text-align:center;max-width:420px}"
           "i{display:block;width:56px;height:6px;background:#F2A541;margin:0 auto 22px}h1{margin:0 0 10px;font-size:26px}p{margin:0;color:#9A9EA6}</style></head>"
           "<body><main><i></i><h1>" +
           title + "</h1><p>" + body + "</p></main></body></html>";
}

namespace {

struct Socket {
    SOCKET s = INVALID_SOCKET;
    ~Socket() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
};

struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
};

bool WaitReadable(SOCKET s, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return select(0, &set, nullptr, nullptr, &tv) > 0;
}

std::string UrlDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+')
            out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                 isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

std::optional<std::string> QueryValue(std::string_view query, std::string_view name) {
    if (!query.empty() && query.front() == '?') query.remove_prefix(1);
    while (!query.empty()) {
        size_t amp = query.find('&');
        std::string_view pair = query.substr(0, amp);
        size_t eq = pair.find('=');
        if (UrlDecode(pair.substr(0, eq)) == name)
            return eq == std::string_view::npos ? std::string() : UrlDecode(pair.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return std::nullopt;
}

void AnswerBrowser(SOCKET browser, const std::string& result) {
    std::string answer;
    auto site = Url::Parse(settings::String("websiteUrl").value_or(""));
    if (site && site->IsHttps()) {
        std::string landing = ResolveUrl(site->Origin() + site->pathAndQuery,
                                         "launcher/signed-in" + (result == "ok" ? std::string() : "?result=" + result));
        answer =
            "HTTP/1.1 302 Found\r\nLocation: " + landing +
            "\r\nContent-Length: 0\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n";
    } else {
        bool ok = result == "ok";
        std::string page = CallbackPage(ok                      ? "You're signed in"
                                        : result == "cancelled" ? "Sign-in cancelled"
                                                                : "Sign-in failed",
                                        ok ? "Head back to RWND Parabellum. You can close this tab."
                                           : "Return to the launcher and try again.");
        answer = std::string("HTTP/1.1 ") + (ok ? "200 OK" : "400 Bad Request") +
                 "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(page.size()) +
                 "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n" + page;
    }
    DWORD timeout = 5000;
    setsockopt(browser, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    send(browser, answer.data(), static_cast<int>(answer.size()), 0);
    shutdown(browser, SD_SEND);
}

}

LoginSession AccountClient::LoopbackLogin(SignInProvider provider, const std::string& verifier,
                                          const std::string& challenge, const CancelToken& cancel) {
    static WinsockInit winsock;
    bool steam = provider == SignInProvider::Steam;
    Socket listener;
    listener.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (listener.s == INVALID_SOCKET || bind(listener.s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 ||
        listen(listener.s, SOMAXCONN) != 0)
        Throw(ErrorKind::IO, "Cannot listen for the sign-in callback.");
    int length = sizeof address;
    getsockname(listener.s, reinterpret_cast<sockaddr*>(&address), &length);
    std::string callback = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/callback/";
    openBrowser_(ResolveUrl(apiBaseUrl_, std::string(steam ? "auth/steam/login" : "auth/discord/login") +
                                             "?returnUrl=" + EscapeDataString(callback) +
                                             "&codeChallenge=" + challenge));
    for (;;) {
        cancel.ThrowIfCanceled();
        if (!WaitReadable(listener.s, 200)) continue;
        Socket browser;
        browser.s = accept(listener.s, nullptr, nullptr);
        if (browser.s == INVALID_SOCKET) continue;
        std::string line;
        auto deadline = GetTickCount64() + 5000;
        bool complete = false;
        while (GetTickCount64() < deadline && line.size() <= 4096) {
            cancel.ThrowIfCanceled();
            if (!WaitReadable(browser.s, 100)) continue;
            char buffer[1024];
            int n = recv(browser.s, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            line.append(buffer, n);
            size_t end = line.find('\n');
            if (end != std::string::npos) {
                line.resize(end);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                complete = true;
                break;
            }
        }
        if (!complete || line.size() > 4096) continue;
        size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
        if (a == std::string::npos || b == std::string::npos || line.find(' ', b + 1) != std::string::npos ||
            line.substr(0, a) != "GET")
            continue;
        std::string target = line.substr(a + 1, b - a - 1);
        size_t q = target.find('?');
        std::string path = target.substr(0, q), query = q == std::string::npos ? "" : target.substr(q);
        if (path != "/callback/") continue;
        if (auto error = QueryValue(query, "error")) {
            bool cancelled = *error == "access_denied";
            AnswerBrowser(browser.s, cancelled ? "cancelled" : "failed");
            if (!steam)
                Throw(ErrorKind::InvalidOperation,
                      cancelled ? "Discord sign-in was cancelled. Try again." : "Discord sign-in failed. Try again.");
            Throw(ErrorKind::InvalidOperation, cancelled                ? "Steam sign-in was cancelled. Try again."
                                               : *error == "restricted" ? "This account or connection is restricted."
                                                                        : "Steam sign-in failed. Try again.");
        }
        auto code = QueryValue(query, "code");
        if (!code || code->size() != 43) continue;
        HttpResponse response;
        try {
            response = Post("auth/token", {{"code", *code}, {"codeVerifier", verifier}, {"rememberMe", true}}, cancel);
        } catch (...) {
            AnswerBrowser(browser.s, "failed");
            throw;
        }
        AnswerBrowser(browser.s, response.IsSuccess() ? "ok" : "failed");
        if (!response.IsSuccess()) ThrowStatus(response.status);
        return LoginSession::FromJson(ParseJson(response.body));
    }
}

SignInProviders AccountClient::Providers(const CancelToken& cancel) {
    SignInProviders providers;
    try {
        HttpRequest request;
        request.url = ResolveUrl(apiBaseUrl_, "auth/providers");
        request.timeoutMs = 5000;
        HttpResponse response = http_->Send(request, cancel);
        if (response.status != 200) return providers;
        Json body = ParseJson(response.body);
        const Json* steam = Field(body, "steam");
        providers.steam = steam && steam->is_boolean() && steam->get<bool>();
    } catch (...) {
        providers.steam = false;
    }
    return providers;
}

LoginSession AccountClient::RedeemCode(const std::string& code, const std::string& codeVerifier,
                                       const CancelToken& cancel) {
    Require(code.size() == 43 && codeVerifier.size() >= 43 && codeVerifier.size() <= 128, "Invalid sign-in code.");
    HttpResponse response =
        Post("auth/token", {{"code", code}, {"codeVerifier", codeVerifier}, {"rememberMe", true}}, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
    return LoginSession::FromJson(ParseJson(response.body));
}

LoginSession AccountClient::Refresh(const std::string& refreshToken, const CancelToken& cancel) {
    HttpResponse response = Post("auth/refresh", {{"refreshToken", refreshToken}}, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
    return LoginSession::FromJson(ParseJson(response.body));
}

void AccountClient::RevokeRefresh(const std::string& refreshToken, const CancelToken& cancel) {
    HttpResponse response = Post("auth/refresh/revoke", {{"refreshToken", refreshToken}}, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
}

PlayerProfile AccountClient::Me(const LoginSession& session, const CancelToken& cancel) {
    HttpResponse response = Authorized("GET", "me", session, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
    return PlayerProfile::FromJson(ParseJson(response.body));
}

bool AccountClient::NeedsInvite(const LoginSession& session, const CancelToken& cancel) {
    try {
        HttpResponse response = SignedGet("social/download-access", session, cancel, 10000);
        if (response.status != 200) return false;
        Json body = ParseJson(response.body);
        const Json* allowed = body.is_object() ? Field(body, "allowed") : nullptr;
        return allowed && allowed->is_boolean() && !allowed->get<bool>();
    } catch (const Error& e) {
        if (e.kind == ErrorKind::Canceled) throw;
        return false;
    } catch (...) {
        return false;
    }
}

static std::string Str(const Json& j, std::string_view name) {
    return GetString(j, name).value_or("");
}
static Json OptStr(const Json& j, std::string_view name) {
    auto v = GetString(j, name);
    return v ? Json(*v) : Json(nullptr);
}

Json AccountClient::News(const CancelToken& cancel) {
    Json feed = GetJson("news", cancel), out = Json::array();
    if (const Json* items = feed.is_object() ? Field(feed, "items") : nullptr; items && items->is_array())
        for (auto& i : *items)
            out.push_back({{"id", Str(i, "id")},
                           {"title", Str(i, "title")},
                           {"body", Str(i, "body")},
                           {"url", OptStr(i, "url")}});
    return out;
}

Json AccountClient::Announcements(const CancelToken& cancel) {
    Json feed = GetJson("announcements", cancel), out = Json::array();
    if (const Json* items = feed.is_object() ? Field(feed, "items") : nullptr; items && items->is_array())
        for (auto& i : *items) {
            Require(i.is_object(), "Invalid announcement.");
            int64_t publish = GetTicks(i, "publishAt").value_or(0);
            out.push_back({{"id", GetGuid(i, "id").value_or(Guid{}).D()},
                           {"title", Str(i, "title")},
                           {"body", Str(i, "body")},
                           {"category", Str(i, "category")},
                           {"banner", GetBool(i, "banner").value_or(false)},
                           {"severity", Str(i, "severity")},
                           {"publishAt", FormatIsoTicks(publish)},
                           {"publishDate", FormatLocalDate(publish)},
                           {"publishTicks", publish}});
        }
    return out;
}

Json AccountClient::PendingPoll(const LoginSession& session, const CancelToken& cancel) {
    Json open = GetJson("polls/open", cancel);
    const Json* polls = open.is_object() ? Field(open, "polls") : nullptr;
    if (!polls || !polls->is_array() || polls->empty()) return nullptr;
    HttpResponse response = Authorized("GET", "social/polls/mine", session, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
    Json mine = ParseJson(response.body);
    std::vector<std::string> voted;
    if (const Json* votes = mine.is_object() ? Field(mine, "votes") : nullptr; votes && votes->is_array())
        for (auto& v : *votes)
            if (v.is_object()) voted.push_back(ToLowerAscii(Str(v, "pollId")));
    for (auto& p : *polls) {
        if (!p.is_object() || Str(p, "id").empty()) continue;
        if (std::ranges::find(voted, ToLowerAscii(Str(p, "id"))) != voted.end()) continue;
        return {{"id", Str(p, "id")}, {"title", Str(p, "title")}, {"question", OptStr(p, "question")}};
    }
    return nullptr;
}

Json AccountClient::Presence(const CancelToken& cancel) {
    Json feed = GetJson("presence", cancel);
    Json out = {{"onlinePlayers", 0}, {"players", Json::array()}};
    if (!feed.is_object()) return out;
    out["onlinePlayers"] = GetInt(feed, "onlinePlayers").value_or(0);
    if (const Json* players = Field(feed, "players"); players && players->is_array())
        for (auto& p : *players) {
            Require(p.is_object(), "Invalid presence.");
            Json match = nullptr;
            if (const Json* m = Field(p, "match"); m && m->is_object()) {
                auto opt = [&](std::string_view n) {
                    auto v = GetInt(*m, n);
                    return v ? Json(*v) : Json(nullptr);
                };
                match = {{"map", Str(*m, "map")},
                         {"playlist", Str(*m, "playlist")},
                         {"round", opt("round")},
                         {"totalRounds", opt("totalRounds")},
                         {"score", opt("score")},
                         {"kills", GetInt(*m, "kills").value_or(0)},
                         {"deaths", GetInt(*m, "deaths").value_or(0)}};
            }
            out["players"].push_back({{"displayName", Str(p, "displayName")},
                                      {"avatarUrl", OptStr(p, "avatarUrl")},
                                      {"activity", Str(p, "activity")},
                                      {"match", match}});
        }
    return out;
}

static Json Person(const Json& link) {
    const Json* p = Field(link, "person");
    if (!p || !p->is_object()) return nullptr;
    return {{"username", Str(*p, "username")},
            {"avatarUrl", OptStr(*p, "avatarUrl")},
            {"online", GetBool(*p, "online").value_or(false)}};
}

Json AccountClient::Friends(const LoginSession& session, const CancelToken& cancel) {
    HttpResponse response = Authorized("GET", "social/friends", session, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
    Json body = ParseJson(response.body);
    Json out = {{"friends", Json::array()}, {"requests", Json::array()}};
    if (!body.is_object()) return out;
    for (auto list : {"friends", "requests"})
        if (const Json* items = Field(body, list); items && items->is_array())
            for (auto& link : *items) {
                Require(link.is_object(), "Invalid friend list.");
                out[list].push_back({{"person", Person(link)}, {"direction", Str(link, "direction")}});
            }
    return out;
}

static void EnsureSocialSuccess(const HttpResponse& response) {
    if (response.IsSuccess()) return;
    Throw(ErrorKind::Http,
          ErrorMessage(response.body).value_or("The server answered " + std::to_string(response.status) + "."),
          response.status);
}

Json AccountClient::SearchPlayers(const LoginSession& session, const std::string& query, const CancelToken& cancel) {
    HttpResponse response = Authorized("GET", "social/search?q=" + EscapeDataString(query), session, cancel);
    EnsureSocialSuccess(response);
    Json body = ParseJson(response.body), out = Json::array();
    if (const Json* people = body.is_object() ? Field(body, "people") : nullptr; people && people->is_array())
        for (auto& p : *people)
            out.push_back({{"username", Str(p, "username")},
                           {"avatarUrl", OptStr(p, "avatarUrl")},
                           {"online", GetBool(p, "online").value_or(false)}});
    return out;
}

void AccountClient::FriendAction(const LoginSession& session, const std::string& action, const std::string& username,
                                 const CancelToken& cancel) {
    if (action != "request" && action != "accept" && action != "remove" && action != "block")
        Throw(ErrorKind::Argument, "Unknown friend action.");
    HttpResponse response =
        Authorized("POST", action == "block" ? std::string("social/block") : "social/friends/" + action, session,
                   cancel, ToJson(Json{{"username", username}}), {{"Idempotency-Key", Guid::New().D()}});
    EnsureSocialSuccess(response);
}

std::optional<ChatTicketInfo> AccountClient::ChatTicket(const LoginSession& session, const CancelToken& cancel) {
    HttpResponse response = Authorized("GET", "social/chat/ticket", session, cancel);
    if (response.status == 404) return std::nullopt;
    if (response.status == 401) Throw(ErrorKind::LoginRequired, "Sign in again.", 401);
    EnsureSocialSuccess(response);
    Json body = ParseJson(response.body);
    ChatTicketInfo info{Str(body, "url"), Str(body, "ticket")};
    Require(!info.url.empty() && !info.ticket.empty(), "Invalid chat ticket.");
    return info;
}

Json AccountClient::ServerChoice(const Json& body) {
    auto word = [](const std::string& s, size_t max, bool upper) {
        if (s.empty() || s.size() > max) return false;
        for (char c : s)
            if (!((c >= 'a' && c <= 'z') || (upper && ((c >= 'A' && c <= 'Z') || c == ' ')))) return false;
        return true;
    };
    Json regions = Json::array(), allowed = Json::array();
    if (const Json* list = body.is_object() ? Field(body, "regions") : nullptr; list && list->is_array())
        for (const auto& r : *list) {
            if (!r.is_object() || regions.size() >= 8) continue;
            std::string id = GetString(r, "id").value_or(""), name = GetString(r, "name").value_or("");
            if (!word(id, 8, false) || !word(name, 16, true)) continue;
            regions.push_back({{"id", id}, {"name", name}});
        }
    Require(!regions.empty(), "Invalid server list.");
    if (const Json* list = Field(body, "allowed"); list && list->is_array())
        for (const auto& a : *list)
            if (a.is_string())
                for (const auto& r : regions)
                    if (r["id"] == a && std::find(allowed.begin(), allowed.end(), a) == allowed.end())
                        allowed.push_back(a);
    if (allowed.empty())
        for (const auto& r : regions) allowed.push_back(r["id"]);
    return {{"regions", regions}, {"allowed", allowed}};
}

std::optional<Json> AccountClient::Servers(const LoginSession& session, const CancelToken& cancel) {
    HttpResponse response = Authorized("GET", "social/servers", session, cancel);
    if (response.status == 404) return std::nullopt;
    if (response.status == 401) Throw(ErrorKind::LoginRequired, "Sign in again.", 401);
    EnsureSocialSuccess(response);
    return ServerChoice(ParseJson(response.body));
}

Json AccountClient::SetServers(const LoginSession& session, const std::vector<std::string>& allowed,
                               const CancelToken& cancel) {
    Json list = Json::array();
    for (const auto& a : allowed) list.push_back(a);
    HttpResponse response = Authorized("POST", "social/servers", session, cancel, ToJson(Json{{"allowed", list}}),
                                       {{"Idempotency-Key", Guid::New().D()}});
    if (response.status == 401) Throw(ErrorKind::LoginRequired, "Sign in again.", 401);
    EnsureSocialSuccess(response);
    return ServerChoice(ParseJson(response.body));
}

std::string AccountClient::SubmitCrashReport(const LoginSession& session, std::string bytes,
                                             const CancelToken& cancel) {
    HttpRequest request;
    request.method = "POST";
    request.url = ResolveUrl(apiBaseUrl_, "launcher/crash-reports");
    request.headers = {{"Authorization", "Bearer " + session.token}};
    request.body = std::move(bytes);
    request.contentType = "application/octet-stream";
    request.timeoutMs =
        static_cast<int>(45000 + std::min<size_t>(request.body ? request.body->size() / 65536 : 0, 600) * 1000);
    HttpResponse response = http_->Send(request, cancel);
    if (!response.IsSuccess())
        Throw(ErrorKind::Http, "RWND could not accept the crash report. You can retry or keep it locally.",
              response.status);
    const std::string reportId = GetString(ParseJson(response.body), "reportId").value_or("");
    Require(reportId.size() == 64 && reportId.find_first_not_of("0123456789abcdef") == std::string::npos,
            "RWND did not acknowledge the crash report.");
    return reportId;
}
void AccountClient::RevokeGameSession(const LoginSession& session, const Guid& id, const CancelToken& cancel) {
    HttpResponse response = Authorized("DELETE", "game/sessions/" + id.D(), session, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
}

}
