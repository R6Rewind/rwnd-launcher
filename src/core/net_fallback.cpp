#include "core/net_fallback.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <memory>

namespace rwnd::net {

namespace {

enum TryState : int { Pending = 0, Reached = 1, Stalled = 2, Over = 3 };

[[noreturn]] void TimedOut() {
    Throw(ErrorKind::Timeout, "The request timed out.");
}

}

Fallback::Fallback(Resolver resolver, Options options) : resolver_(std::move(resolver)), options_(std::move(options)) {}

int Fallback::ReachBudget(int timeoutMs) const {
    return std::max(1, std::min(options_.reachBudgetMs, timeoutMs / 2));
}

std::optional<std::string> Fallback::Pinned(const std::string& host, int port) const {
    std::lock_guard lock(mutex_);
    auto it = pinned_.find(ToLowerAscii(host) + ":" + std::to_string(port));
    return it == pinned_.end() ? std::nullopt : std::optional(it->second);
}

void Fallback::Forget() {
    std::lock_guard lock(mutex_);
    pinned_.clear();
}

HttpResponse Fallback::Run(const std::string& host, int port, int timeoutMs, const Attempt& attempt,
                           const CancelToken& cancel) {
    cancel.ThrowIfCanceled();
    const std::string key = ToLowerAscii(host) + ":" + std::to_string(port);
    CancelSource overall(cancel);
    overall.CancelAfter(timeoutMs);
    struct EndTimer {
        CancelSource& s;
        ~EndTimer() { s.Cancel(); }
    } endTimer{overall};

    const bool pinnable = !IsAddressLiteral(host) && options_.mayPin();
    const std::optional<std::string> remembered = pinnable ? Pinned(host, port) : std::nullopt;
    std::vector<std::string> routes{remembered.value_or("")};
    bool expanded = false;
    std::optional<Error> last;

    for (size_t i = 0; i < routes.size(); i++) {
        const std::string pin = routes[i];
        auto trySource = std::make_shared<CancelSource>(overall.Token());
        auto state = std::make_shared<std::atomic<int>>(Pending);
        CancelSource watchdog;
        if (pinnable) {
            watchdog.CancelAfter(ReachBudget(timeoutMs));
            watchdog.Token().Register([state, trySource] {
                int expected = Pending;
                if (state->compare_exchange_strong(expected, Stalled)) trySource->Cancel();
            });
        }
        struct EndTry {
            std::shared_ptr<std::atomic<int>> state;
            CancelSource& watchdog;
            ~EndTry() {
                int expected = Pending;
                state->compare_exchange_strong(expected, Over);
                watchdog.Cancel();
            }
        } endTry{state, watchdog};
        auto reached = [state] {
            int expected = Pending;
            state->compare_exchange_strong(expected, Reached);
        };

        TryReport report;
        try {
            HttpResponse response = attempt(Try{pin}, trySource->Token(), reached, report);
            if (!pin.empty() && pin != remembered) {
                std::lock_guard lock(mutex_);
                pinned_[key] = pin;
            }
            return response;
        } catch (const Error& e) {
            if (cancel.IsCanceled()) Throw(ErrorKind::Canceled, "The operation was canceled.");
            const bool stalled = state->load() == Stalled;
            if (!stalled && overall.Token().IsCanceled()) TimedOut();
            const bool pathProblem = stalled || (report.pathFailure && state->load() != Reached);
            if (!pathProblem || !pinnable) throw;
            last = stalled ? Error(ErrorKind::Timeout, "The request timed out.") : e;
            if (!pin.empty() && pin == remembered) {
                std::lock_guard lock(mutex_);
                pinned_.erase(key);
            }
        }
        if (!expanded) {
            expanded = true;
            if (!pin.empty()) routes.push_back("");
            std::vector<Address> addresses;
            try {
                addresses = resolver_(host);
            } catch (const Error&) {
            }
            int added = 0;
            for (auto& a : addresses) {
                if (a.family != Family::V4 || a.text == pin || added >= options_.maxIpv4Tries) continue;
                if (std::find(routes.begin(), routes.end(), a.text) != routes.end()) continue;
                routes.push_back(a.text);
                added++;
            }
        }
    }
    if (last) throw *last;
    TimedOut();
}

namespace {
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
};
}

std::vector<Address> SystemResolve(const std::string& host) {
    static WinsockInit winsock;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return {};
    std::vector<Address> out;
    for (auto* a = result; a; a = a->ai_next) {
        char text[INET6_ADDRSTRLEN] = {};
        Address address;
        if (a->ai_family == AF_INET && a->ai_addrlen >= sizeof(sockaddr_in)) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, text, sizeof text);
            address.family = Family::V4;
        } else if (a->ai_family == AF_INET6 && a->ai_addrlen >= sizeof(sockaddr_in6)) {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr, text, sizeof text);
            address.family = Family::V6;
        } else {
            continue;
        }
        address.text = text;
        if (!address.text.empty() &&
            std::none_of(out.begin(), out.end(), [&](const Address& o) { return o.text == address.text; }))
            out.push_back(address);
    }
    freeaddrinfo(result);
    return out;
}

bool IsAddressLiteral(const std::string& host) {
    if (host.empty() || host.front() == '[' || host.find(':') != std::string::npos) return true;
    std::string lower = ToLowerAscii(host);
    if (lower == "localhost" || EndsWith(lower, ".localhost")) return true;
    in_addr v4{};
    return inet_pton(AF_INET, host.c_str(), &v4) == 1;
}

bool ProxyConfigured() {
    bool proxy = false;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG user{};
    if (WinHttpGetIEProxyConfigForCurrentUser(&user)) {
        proxy = (user.lpszProxy && *user.lpszProxy) || (user.lpszAutoConfigUrl && *user.lpszAutoConfigUrl);
        for (auto* s : {user.lpszProxy, user.lpszProxyBypass, user.lpszAutoConfigUrl})
            if (s) GlobalFree(s);
    }
    WINHTTP_PROXY_INFO machine{};
    if (WinHttpGetDefaultProxyConfiguration(&machine)) {
        proxy = proxy ||
                (machine.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && machine.lpszProxy && *machine.lpszProxy);
        for (auto* s : {machine.lpszProxy, machine.lpszProxyBypass})
            if (s) GlobalFree(s);
    }
    return proxy;
}

}
