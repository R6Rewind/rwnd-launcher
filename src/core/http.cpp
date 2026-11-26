#include "core/http.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include "core/json.h"
#include "core/net_fallback.h"

namespace rwnd {

const std::string* HttpResponse::Header(std::string_view name) const {
    std::string lower = ToLowerAscii(name);
    for (auto& [k, v] : headers)
        if (k == lower) return &v;
    return nullptr;
}

HttpResponse HttpHandler::Stream(const HttpRequest& request, const std::function<void(const HttpResponse&)>& onHeaders,
                                 const std::function<void(const uint8_t*, size_t)>& onData, const CancelToken& cancel) {
    HttpResponse response = Send(request, cancel);
    onHeaders(response);
    onData(reinterpret_cast<const uint8_t*>(response.body.data()), response.body.size());
    response.body.clear();
    return response;
}

std::optional<Url> Url::Parse(std::string_view text) {
    std::string s = Trim(text);
    Url u;
    size_t colon = s.find("://");
    if (colon == std::string::npos) return std::nullopt;
    u.scheme = ToLowerAscii(s.substr(0, colon));
    if (u.scheme != "http" && u.scheme != "https") return std::nullopt;
    size_t hostStart = colon + 3;
    size_t pathStart = s.find_first_of("/?#", hostStart);
    std::string authority =
        s.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    if (authority.empty() || authority.find('@') != std::string::npos || authority.find(' ') != std::string::npos)
        return std::nullopt;
    u.port = u.scheme == "https" ? 443 : 80;
    if (authority.front() == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) return std::nullopt;
        u.host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return std::nullopt;
            u.port = std::atoi(authority.c_str() + close + 2);
        }
    } else {
        size_t portColon = authority.rfind(':');
        u.host = ToLowerAscii(authority.substr(0, portColon));
        if (portColon != std::string::npos) {
            std::string port = authority.substr(portColon + 1);
            if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
                return std::nullopt;
            u.port = std::atoi(port.c_str());
        }
    }
    if (u.host.empty() || u.port <= 0 || u.port > 65535) return std::nullopt;
    u.pathAndQuery = pathStart == std::string::npos ? "/" : s.substr(pathStart);
    if (u.pathAndQuery.front() != '/') u.pathAndQuery = "/" + u.pathAndQuery;
    size_t hash = u.pathAndQuery.find('#');
    if (hash != std::string::npos) u.pathAndQuery.resize(hash);
    return u;
}

bool Url::IsLoopback() const {
    return host == "localhost" || host == "[::1]" || StartsWith(host, "127.");
}

std::string Url::Origin() const {
    bool defaultPort = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    return scheme + "://" + host + (defaultPort ? "" : ":" + std::to_string(port));
}

std::string ResolveUrl(std::string_view baseUrl, std::string_view relative) {
    if (relative.find("://") != std::string_view::npos) return std::string(relative);
    auto base = Url::Parse(baseUrl);
    if (!base) Throw(ErrorKind::Argument, "Invalid base address.");
    if (!relative.empty() && relative.front() == '/') return base->Origin() + std::string(relative);
    std::string path = base->pathAndQuery;
    size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    if (relative.empty()) return base->Origin() + path;
    if (relative.front() == '?') return base->Origin() + path + std::string(relative);
    path.resize(path.rfind('/') + 1);
    std::string merged = path + std::string(relative);
    size_t q = merged.find('?');
    std::string p = merged.substr(0, q), rest = q == std::string::npos ? "" : merged.substr(q);
    std::vector<std::string> segments;
    size_t at = 1;
    while (at <= p.size()) {
        size_t next = p.find('/', at);
        std::string seg = p.substr(at, next == std::string::npos ? std::string::npos : next - at);
        if (seg == "..") {
            if (!segments.empty()) segments.pop_back();
            if (next == std::string::npos) segments.push_back("");
        } else if (seg == ".") {
            if (next == std::string::npos) segments.push_back("");
        } else
            segments.push_back(seg);
        if (next == std::string::npos) break;
        at = next + 1;
    }
    std::string out;
    for (auto& s : segments) out += "/" + s;
    if (out.empty()) out = "/";
    return base->Origin() + out + rest;
}

std::string EscapeDataString(std::string_view text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

[[noreturn]] void ThrowStatus(int status) {
    static const std::pair<int, const char*> reasons[] = {{400, "Bad Request"},       {401, "Unauthorized"},
                                                          {403, "Forbidden"},         {404, "Not Found"},
                                                          {409, "Conflict"},          {410, "Gone"},
                                                          {429, "Too Many Requests"}, {500, "Internal Server Error"},
                                                          {502, "Bad Gateway"},       {503, "Service Unavailable"}};
    std::string reason;
    for (auto& [code, text] : reasons)
        if (code == status) reason = std::string(" (") + text + ")";
    Throw(ErrorKind::Http, "Response status code does not indicate success: " + std::to_string(status) + reason + ".",
          status);
}

std::optional<std::string> ErrorMessage(const std::string& body) {
    try {
        Json json = ParseJson(body);
        if (json.is_object()) return GetString(json, "error");
    } catch (const Error&) {
    }
    return std::nullopt;
}

namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

constexpr int ConnectTimeoutMs = 4000;
constexpr int MaxRedirects = 10;

bool IsRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

struct Progress {
    const std::function<void()>* reached = nullptr;
};

void CALLBACK OnStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID, DWORD) {
    if (!context ||
        (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST && status != WINHTTP_CALLBACK_STATUS_REQUEST_SENT))
        return;
    auto* progress = reinterpret_cast<Progress*>(context);
    if (progress->reached && *progress->reached) (*progress->reached)();
}

class WinHttpHandler : public HttpHandler {
public:
    explicit WinHttpHandler(const net::WinHttpTestSetup* setup = nullptr)
        : systemRoute_(setup ? setup->systemRoute : std::string()),
          fallback_(setup && setup->resolver ? setup->resolver : net::Resolver(net::SystemResolve),
                    MakeOptions(setup)) {
        std::wstring agent = L"RWND-Parabellum/" + Widen(DisplayVersion());
        session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)
            session_ = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (session_ && !WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        if (session_) {
            BOOL on = TRUE;
            WinHttpSetOption(session_, WINHTTP_OPTION_IPV6_FAST_FALLBACK, &on, sizeof on);
            pinning_ = CanPin(session_);
        }
    }
    ~WinHttpHandler() override {
        if (session_) WinHttpCloseHandle(session_);
    }

    HttpResponse Send(const HttpRequest& request, const CancelToken& cancel) override {
        std::string body;
        auto response = Run(
            request, nullptr,
            [&](const uint8_t* data, size_t size) {
                if (body.size() + size > 256ull * 1024 * 1024)
                    Throw(ErrorKind::InvalidData, "The response is too large.");
                body.append(reinterpret_cast<const char*>(data), size);
            },
            cancel);
        response.body = std::move(body);
        return response;
    }

    HttpResponse Stream(const HttpRequest& request, const std::function<void(const HttpResponse&)>& onHeaders,
                        const std::function<void(const uint8_t*, size_t)>& onData, const CancelToken& cancel) override {
        return Run(request, &onHeaders, onData, cancel);
    }

private:
    HINTERNET session_ = nullptr;
    std::atomic<bool> pinning_{false};
    std::mutex proxyMutex_;
    ULONGLONG proxyCheckedAt_ = 0;
    bool proxy_ = false;
    std::string systemRoute_;
    net::Fallback fallback_;

    net::Options MakeOptions(const net::WinHttpTestSetup* setup) {
        net::Options options = setup ? setup->options : net::Options{};
        options.mayPin = [this] { return MayPin(); };
        return options;
    }

    bool MayPin() {
        if (!pinning_) return false;
        std::lock_guard lock(proxyMutex_);
        ULONGLONG now = GetTickCount64();
        if (!proxyCheckedAt_ || now - proxyCheckedAt_ > 60000) {
            proxy_ = net::ProxyConfigured();
            proxyCheckedAt_ = now;
        }
        return !proxy_;
    }

    static bool CanPin(HINTERNET session) {
        Handle connection;
        connection.h = WinHttpConnect(session, L"localhost", 80, 0);
        if (!connection.h) return false;
        Handle request;
        request.h = WinHttpOpenRequest(connection.h, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!request.h) return false;
        wchar_t pin[] = L"127.0.0.1";
        return WinHttpSetOption(request.h, WINHTTP_OPTION_RESOLUTION_HOSTNAME, pin,
                                static_cast<DWORD>(wcslen(pin) * sizeof(wchar_t))) != FALSE;
    }

    HttpResponse Run(const HttpRequest& original, const std::function<void(const HttpResponse&)>* onHeaders,
                     const std::function<void(const uint8_t*, size_t)>& onData, const CancelToken& cancel) {
        cancel.ThrowIfCanceled();
        if (!session_) Throw(ErrorKind::Http, "Windows networking is unavailable.");
        HttpRequest request = original;
        const auto started = std::chrono::steady_clock::now();
        for (int hop = 0;; hop++) {
            auto url = Url::Parse(request.url);
            if (!url) Throw(ErrorKind::Argument, "Invalid address: " + request.url);
            std::string host = url->host.front() == '[' ? url->host.substr(1, url->host.size() - 2) : url->host;
            std::optional<std::string> location;
            HttpResponse response = fallback_.Run(
                host, url->port, request.timeoutMs,
                [&](const net::Try& route, const CancelToken& abort, const std::function<void()>& reached,
                    net::TryReport& report) {
                    location.reset();
                    return RunOnce(request, *url, route.pin, abort, reached, report, onHeaders, onData, location);
                },
                cancel);
            if (!location) return response;
            if (hop >= MaxRedirects) Throw(ErrorKind::Http, "The server redirected too many times.");
            std::string next = ResolveUrl(request.url, *location);
            auto target = Url::Parse(next);
            if (!target) Throw(ErrorKind::Http, "The server sent an invalid redirect.");
            if (target->Origin() != url->Origin())
                std::erase_if(request.headers, [](const auto& h) { return ToLowerAscii(h.first) == "authorization"; });
            if (response.status == 303 ||
                ((response.status == 301 || response.status == 302) && request.method == "POST")) {
                request.method = "GET";
                request.body.reset();
            }
            request.url = next;
            auto spent =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                    .count();
            request.timeoutMs = static_cast<int>(std::max<long long>(1, original.timeoutMs - spent));
        }
    }

    HttpResponse RunOnce(const HttpRequest& request, const Url& url, const std::string& pin, const CancelToken& abort,
                         const std::function<void()>& reached, net::TryReport& report,
                         const std::function<void(const HttpResponse&)>* onHeaders,
                         const std::function<void(const uint8_t*, size_t)>& onData,
                         std::optional<std::string>& location) {
        abort.ThrowIfCanceled();
        const std::string route = pin.empty() ? systemRoute_ : pin;
        Handle connection;
        std::wstring host = Widen(url.host.front() == '[' ? url.host.substr(1, url.host.size() - 2) : url.host);
        connection.h = WinHttpConnect(session_, host.c_str(), static_cast<INTERNET_PORT>(url.port), 0);
        if (!connection.h) NetworkError(nullptr);
        HINTERNET req = WinHttpOpenRequest(connection.h, Widen(request.method).c_str(), Widen(url.pathAndQuery).c_str(),
                                           nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           url.IsHttps() ? WINHTTP_FLAG_SECURE : 0);
        if (!req) NetworkError(nullptr);

        Progress progress{&reached};
        auto closed = std::make_shared<std::atomic<bool>>(false);
        auto closeOnce = [req, closed] {
            if (!closed->exchange(true)) WinHttpCloseHandle(req);
        };
        struct Finish {
            std::function<void()> f;
            ~Finish() { f(); }
        } finish{closeOnce};
        CancelRegistration onAbort(abort, closeOnce);

        auto failed = [&]() -> void {
            if (abort.IsCanceled()) Throw(ErrorKind::Canceled, "The operation was canceled.");
            NetworkError(&report);
        };

        int t = request.timeoutMs;
        WinHttpSetTimeouts(req, t, std::min(t, ConnectTimeoutMs), t, t);
        if (!route.empty()) {
            std::wstring address = Widen(route);
            if (!WinHttpSetOption(req, WINHTTP_OPTION_RESOLUTION_HOSTNAME, address.data(),
                                  static_cast<DWORD>(address.size() * sizeof(wchar_t)))) {
                pinning_ = false;
                report.pathFailure = true;
                Throw(ErrorKind::Http, "No connection could be made.", 0);
            }
        }
        const bool manualRedirects = request.followRedirects && !route.empty();
        if (!request.followRedirects || manualRedirects) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        }
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&progress);
        WinHttpSetOption(req, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context);
        WinHttpSetStatusCallback(req, OnStatus, WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0);

        std::wstring headers;
        for (auto& [k, v] : request.headers) headers += Widen(k) + L": " + Widen(v) + L"\r\n";
        if (request.noCache) headers += L"Cache-Control: no-cache\r\n";
        if (request.body) headers += L"Content-Type: " + Widen(request.contentType) + L"\r\n";
        const std::string& payload = request.body ? *request.body : std::string();
        if (!WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                static_cast<DWORD>(-1),
                                payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data()),
                                static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0))
            failed();
        reached();
        if (!WinHttpReceiveResponse(req, nullptr)) failed();

        HttpResponse response;
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(status);
        size = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (size) {
            std::wstring raw(size / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                    &size, WINHTTP_NO_HEADER_INDEX)) {
                std::string text = Narrow(raw.c_str());
                size_t at = text.find("\r\n");
                while (at != std::string::npos) {
                    size_t next = text.find("\r\n", at + 2);
                    std::string line =
                        text.substr(at + 2, next == std::string::npos ? std::string::npos : next - at - 2);
                    size_t colon = line.find(':');
                    if (colon != std::string::npos)
                        response.headers.emplace_back(ToLowerAscii(Trim(line.substr(0, colon))),
                                                      Trim(line.substr(colon + 1)));
                    at = next;
                }
            }
        }
        if (auto length = response.Header("content-length")) {
            try {
                response.contentLength = std::stoull(*length);
            } catch (...) {
            }
        }
        DWORD urlSize = 0;
        WinHttpQueryOption(req, WINHTTP_OPTION_URL, nullptr, &urlSize);
        if (urlSize) {
            std::wstring finalUrl(urlSize / sizeof(wchar_t), L'\0');
            if (WinHttpQueryOption(req, WINHTTP_OPTION_URL, finalUrl.data(), &urlSize))
                response.finalUrl = Narrow(finalUrl.c_str());
        }
        if (response.finalUrl.empty()) response.finalUrl = request.url;
        if (manualRedirects && IsRedirect(response.status)) {
            if (auto target = response.Header("location"); target && !target->empty()) {
                auto next = Url::Parse(ResolveUrl(request.url, *target));
                if (next && !(url.IsHttps() && !next->IsHttps())) {
                    location = *target;
                    return response;
                }
            }
        }
        if (onHeaders) (*onHeaders)(response);

        std::vector<uint8_t> buffer(1 << 16);
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(req, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) failed();
            if (read == 0) break;
            onData(buffer.data(), read);
        }
        return response;
    }

    [[noreturn]] static void NetworkError(net::TryReport* report) {
        DWORD code = GetLastError();
        std::string reason;
        bool path = false;
        switch (code) {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
                reason = "No such host is known.";
                break;
            case ERROR_WINHTTP_CANNOT_CONNECT:
                reason = "No connection could be made.";
                path = true;
                break;
            case ERROR_WINHTTP_TIMEOUT:
                if (report) report->pathFailure = true;
                Throw(ErrorKind::Timeout, "The request timed out.");
            case ERROR_WINHTTP_SECURE_FAILURE:
                reason = "The secure connection failed.";
                break;
            case ERROR_WINHTTP_CONNECTION_ERROR:
                reason = "The connection was reset.";
                path = true;
                break;
            default:
                reason = "Network error " + std::to_string(code) + ".";
        }
        if (report && path) report->pathFailure = true;
        Throw(ErrorKind::Http, reason, 0);
    }
};

}

std::shared_ptr<HttpHandler> WinHttp() {
    static std::shared_ptr<HttpHandler> shared = std::make_shared<WinHttpHandler>();
    return shared;
}

std::shared_ptr<HttpHandler> net::NewWinHttpForTests(const WinHttpTestSetup& setup) {
    return std::make_shared<WinHttpHandler>(&setup);
}

}
