#include "core/chat_client.h"

#include <algorithm>
#include <chrono>

namespace rwnd {

static int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static std::string Text(const Json& j, std::string_view name) {
    return GetString(j, name).value_or("");
}

ChatClient::ChatClient(std::shared_ptr<HttpHandler> http, TicketFn ticket, EmitFn emit)
    : http_(std::move(http)), ticket_(std::move(ticket)), emit_(std::move(emit)) {}

ChatClient::~ChatClient() {
    Stop();
}

void ChatClient::Start() {
    std::lock_guard lock(mutex_);
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { Run(); });
}

void ChatClient::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    stop_.Cancel();
    if (thread_.joinable()) thread_.join();
    std::string session, url;
    {
        std::lock_guard lock(mutex_);
        session = session_;
        url = url_;
    }
    if (!session.empty()) {
        CancelSource bye;
        bye.CancelAfter(2000);
        try {
            Call("POST", "/chat/v1/bye", Json::object(), bye.Token(), 2000);
        } catch (const Error&) {
        }
    }
}

void ChatClient::RefreshSoon() {
    std::lock_guard lock(mutex_);
    renewAt_ = 0;
}

void ChatClient::Run() {
    int backoff = 2000;
    while (!stop_.Token().IsCanceled()) {
        bool ok = false;
        try {
            ok = Step(stop_.Token());
        } catch (const Error& e) {
            if (e.kind == ErrorKind::Canceled && stop_.Token().IsCanceled()) break;
            if (e.status == 401) Drop();
            bool changed = false;
            {
                std::lock_guard lock(mutex_);
                changed = connected_;
                connected_ = false;
            }
            if (changed) Changed();
        } catch (const std::exception&) {
        }
        if (ok) {
            backoff = 2000;
            continue;
        }
        bool off;
        {
            std::lock_guard lock(mutex_);
            off = !available_;
        }
        if (!stop_.Token().Sleep(off ? 300000 : backoff)) break;
        backoff = std::min(backoff * 2, 30000);
    }
}

bool ChatClient::Step(const CancelToken& cancel) {
    bool connect, renew;
    {
        std::lock_guard lock(mutex_);
        connect = session_.empty();
        renew = !connect && NowSeconds() >= renewAt_;
    }
    if (connect) {
        Connect(cancel);
        std::lock_guard lock(mutex_);
        return available_;
    }
    if (renew) Renew(cancel);
    Poll(cancel);
    return true;
}

void ChatClient::Connect(const CancelToken& cancel) {
    std::optional<ChatTicketInfo> ticket = ticket_(cancel);
    if (!ticket) {
        bool changed;
        {
            std::lock_guard lock(mutex_);
            changed = available_ || !offKnown_;
            available_ = false;
            connected_ = false;
            offKnown_ = true;
            friends_ = Json::array();
        }
        if (changed) Changed();
        return;
    }
    auto url = Url::Parse(ticket->url);
    if (!url || (!url->IsHttps() && !url->IsLoopback()))
        Throw(ErrorKind::InvalidData, "The chat address is not secure.");
    {
        std::lock_guard lock(mutex_);
        url_ = ticket->url;
        while (!url_.empty() && url_.back() == '/') url_.pop_back();
        available_ = true;
    }
    HttpResponse response =
        Call("POST", "/chat/v1/hello", Json{{"ticket", ticket->ticket}, {"client", "launcher"}}, cancel);
    Check(response);
    Json body = ParseJson(response.body);
    {
        std::lock_guard lock(mutex_);
        session_ = Text(body, "session");
        cursor_ = GetInt(body, "cursor").value_or(0);
        const Json* friends = Field(body, "friends");
        friends_ = friends && friends->is_array() ? *friends : Json::array();
        connected_ = !session_.empty();
        renewAt_ = NowSeconds() + RenewSeconds;
    }
    Changed();
}

void ChatClient::Renew(const CancelToken& cancel) {
    std::optional<ChatTicketInfo> ticket = ticket_(cancel);
    if (!ticket) {
        Drop();
        return;
    }
    HttpResponse response = Call("POST", "/chat/v1/renew", Json{{"ticket", ticket->ticket}}, cancel);
    Check(response);
    Json body = ParseJson(response.body);
    {
        std::lock_guard lock(mutex_);
        if (const Json* friends = Field(body, "friends"); friends && friends->is_array()) friends_ = *friends;
        renewAt_ = NowSeconds() + RenewSeconds;
    }
    Changed();
}

void ChatClient::Poll(const CancelToken& cancel) {
    int64_t after;
    {
        std::lock_guard lock(mutex_);
        after = cursor_;
    }
    HttpResponse response =
        Call("GET", "/chat/v1/poll?after=" + std::to_string(after), std::nullopt, cancel, PollTimeoutMs);
    Check(response);
    Json body = ParseJson(response.body);
    bool reload = false;
    if (const Json* events = Field(body, "events"); events && events->is_array())
        for (auto& e : *events) {
            const std::string t = Text(e, "t");
            if (t == "friends" || t == "reset") reload = true;
        }
    Apply(body);
    if (reload) LoadState(cancel);
}

void ChatClient::LoadState(const CancelToken& cancel) {
    HttpResponse response = Call("GET", "/chat/v1/state", std::nullopt, cancel);
    Check(response);
    Json body = ParseJson(response.body);
    {
        std::lock_guard lock(mutex_);
        if (const Json* friends = Field(body, "friends"); friends && friends->is_array()) friends_ = *friends;
        if (auto cursor = GetInt(body, "cursor")) cursor_ = std::max(cursor_, *cursor);
    }
    Changed();
}

void ChatClient::Apply(const Json& poll) {
    std::vector<Json> messages;
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        if (auto cursor = GetInt(poll, "cursor")) cursor_ = std::max(cursor_, *cursor);
        const Json* events = Field(poll, "events");
        if (!events || !events->is_array()) return;
        auto find = [&](const std::string& name) -> Json* {
            for (auto& f : friends_)
                if (IEqualsA(Text(f, "name"), name)) return &f;
            return nullptr;
        };
        for (auto& e : *events) {
            const std::string t = Text(e, "t");
            if (t == "msg") {
                Json m = {{"with", Text(e, "with")},
                          {"id", GetInt(e, "id").value_or(0)},
                          {"mine", GetBool(e, "mine").value_or(false)},
                          {"text", Text(e, "text")},
                          {"at", Text(e, "at")}};
                if (Json* f = find(m["with"].get<std::string>())) {
                    (*f)["last"] = {{"text", m["text"]}, {"mine", m["mine"]}, {"at", m["at"]}};
                    if (!m["mine"].get<bool>())
                        (*f)["unread"] = GetInt(*f, "unread").value_or(0) + 1;
                    else
                        (*f)["unread"] = 0;
                }
                messages.push_back(std::move(m));
                changed = true;
            } else if (t == "presence") {
                if (Json* f = find(Text(e, "name"))) {
                    (*f)["online"] = GetBool(e, "online").value_or(false);
                    (*f)["game"] = GetBool(e, "game").value_or(false);
                    changed = true;
                }
            } else if (t == "read") {
                if (Json* f = find(Text(e, "with"))) {
                    (*f)["unread"] = 0;
                    changed = true;
                }
            }
        }
    }
    for (auto& m : messages) emit_("chat.message", m);
    if (changed) Changed();
}

Json ChatClient::State() {
    std::lock_guard lock(mutex_);
    return {{"available", available_}, {"connected", connected_}, {"friends", friends_}};
}

Json ChatClient::History(const std::string& with, std::optional<int64_t> before, const CancelToken& cancel) {
    std::string path = "/chat/v1/history?with=" + EscapeDataString(with);
    if (before) path += "&before=" + std::to_string(*before);
    HttpResponse response = Call("GET", path, std::nullopt, cancel);
    Check(response);
    Json body = ParseJson(response.body), items = Json::array();
    if (const Json* list = Field(body, "items"); list && list->is_array())
        for (auto& m : *list)
            items.push_back({{"with", Text(body, "with")},
                             {"id", GetInt(m, "id").value_or(0)},
                             {"mine", GetBool(m, "mine").value_or(false)},
                             {"text", Text(m, "text")},
                             {"at", Text(m, "at")}});
    return {{"items", items}, {"more", GetBool(body, "more").value_or(false)}};
}

Json ChatClient::Send(const std::string& to, const std::string& text, const CancelToken& cancel) {
    if (text.empty()) Throw(ErrorKind::Argument, "Type a message.");
    HttpResponse response = Call("POST", "/chat/v1/send", Json{{"to", to}, {"text", text}}, cancel);
    Check(response);
    Json m = ParseJson(response.body);
    return {{"with", Text(m, "with")},
            {"id", GetInt(m, "id").value_or(0)},
            {"mine", true},
            {"text", Text(m, "text")},
            {"at", Text(m, "at")}};
}

void ChatClient::Read(const std::string& with, int64_t upTo, const CancelToken& cancel) {
    Check(Call("POST", "/chat/v1/read", Json{{"with", with}, {"upTo", upTo}}, cancel));
    std::lock_guard lock(mutex_);
    for (auto& f : friends_)
        if (IEqualsA(Text(f, "name"), with)) f["unread"] = 0;
}

HttpResponse ChatClient::Call(const std::string& method, const std::string& path, std::optional<Json> body,
                              const CancelToken& cancel, int timeoutMs) {
    HttpRequest request;
    {
        std::lock_guard lock(mutex_);
        if (url_.empty() || (path != "/chat/v1/hello" && session_.empty()))
            Throw(ErrorKind::InvalidOperation, "Messages are connecting. Try again in a moment.");
        request.url = url_ + path;
        if (path != "/chat/v1/hello") request.headers.push_back({"Authorization", "Bearer " + session_});
    }
    request.method = method;
    if (body) request.body = ToJson(*body);
    request.timeoutMs = timeoutMs;
    request.noCache = true;
    HttpResponse response = http_->Send(request, cancel);
    if (response.status == 401 && path != "/chat/v1/bye") Drop();
    return response;
}

void ChatClient::Check(const HttpResponse& response) {
    if (response.IsSuccess()) return;
    Throw(ErrorKind::Http, ErrorMessage(response.body).value_or("Messages are unavailable right now."),
          response.status);
}

void ChatClient::Drop() {
    std::lock_guard lock(mutex_);
    session_.clear();
    cursor_ = 0;
}

void ChatClient::Changed() {
    emit_("chat.changed", State());
}

}
