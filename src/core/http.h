#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/util.h"

namespace rwnd {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::optional<std::string> body;
    std::string contentType = "application/json; charset=utf-8";
    bool followRedirects = false;
    int timeoutMs = 20000;
    bool noCache = false;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string finalUrl;
    std::optional<uint64_t> contentLength;
    bool IsSuccess() const { return status >= 200 && status <= 299; }
    const std::string* Header(std::string_view name) const;
};

class HttpHandler {
public:
    virtual ~HttpHandler() = default;
    virtual HttpResponse Send(const HttpRequest& request, const CancelToken& cancel) = 0;
    virtual HttpResponse Stream(const HttpRequest& request, const std::function<void(const HttpResponse&)>& onHeaders,
                                const std::function<void(const uint8_t*, size_t)>& onData, const CancelToken& cancel);
};

std::shared_ptr<HttpHandler> WinHttp();

struct Url {
    std::string scheme, host, pathAndQuery;
    int port = 0;
    static std::optional<Url> Parse(std::string_view text);
    bool IsHttps() const { return scheme == "https"; }
    bool IsLoopback() const;
    std::string Origin() const;
};
std::string ResolveUrl(std::string_view baseUrl, std::string_view relative);
std::string EscapeDataString(std::string_view text);
[[noreturn]] void ThrowStatus(int status);
std::optional<std::string> ErrorMessage(const std::string& body);

}
