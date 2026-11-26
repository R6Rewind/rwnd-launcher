#pragma once
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include "core/http.h"
#include "core/json.h"
#include "core/util.h"

namespace rwnd {

struct ChatTicketInfo {
    std::string url, ticket;
};

class ChatClient {
public:
    using TicketFn = std::function<std::optional<ChatTicketInfo>(const CancelToken&)>;
    using EmitFn = std::function<void(const std::string& event, const Json& data)>;

    ChatClient(std::shared_ptr<HttpHandler> http, TicketFn ticket, EmitFn emit);
    ~ChatClient();
    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;

    void Start();
    void Stop();
    void RefreshSoon();

    Json State();
    Json History(const std::string& with, std::optional<int64_t> before, const CancelToken& cancel);
    Json Send(const std::string& to, const std::string& text, const CancelToken& cancel);
    void Read(const std::string& with, int64_t upTo, const CancelToken& cancel);

    bool Step(const CancelToken& cancel);
    void Apply(const Json& poll);

    static constexpr int PollTimeoutMs = 40000;
    static constexpr int64_t RenewSeconds = 600;
    static constexpr size_t MaxText = 500;

private:
    void Run();
    HttpResponse Call(const std::string& method, const std::string& path, std::optional<Json> body,
                      const CancelToken& cancel, int timeoutMs = 15000);
    void Connect(const CancelToken& cancel);
    void Renew(const CancelToken& cancel);
    void Poll(const CancelToken& cancel);
    void LoadState(const CancelToken& cancel);
    void Changed();
    void Drop();
    static void Check(const HttpResponse& response);

    std::shared_ptr<HttpHandler> http_;
    TicketFn ticket_;
    EmitFn emit_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    CancelSource stop_;
    bool running_ = false;
    bool available_ = true, connected_ = false, offKnown_ = false;
    std::string url_, session_;
    int64_t cursor_ = 0, renewAt_ = 0;
    Json friends_ = Json::array();
};

}
