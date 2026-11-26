#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/http.h"
#include "core/util.h"

namespace rwnd::net {

enum class Family { V4, V6 };
struct Address {
    Family family = Family::V4;
    std::string text;
};

struct Try {
    std::string pin;
};
struct TryReport {
    bool pathFailure = false;
};

using Resolver = std::function<std::vector<Address>(const std::string& host)>;
using Attempt = std::function<HttpResponse(const Try& route, const CancelToken& abort,
                                           const std::function<void()>& reached, TryReport& report)>;

struct Options {
    int reachBudgetMs = 4000;
    int maxIpv4Tries = 2;
    std::function<bool()> mayPin = [] { return true; };
};

class Fallback {
public:
    explicit Fallback(Resolver resolver, Options options = {});

    HttpResponse Run(const std::string& host, int port, int timeoutMs, const Attempt& attempt,
                     const CancelToken& cancel);

    std::optional<std::string> Pinned(const std::string& host, int port) const;
    void Forget();

    int ReachBudget(int timeoutMs) const;

private:
    Resolver resolver_;
    Options options_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> pinned_;
};

struct WinHttpTestSetup {
    Resolver resolver;
    Options options;
    std::string systemRoute;
};
std::shared_ptr<HttpHandler> NewWinHttpForTests(const WinHttpTestSetup& setup);

std::vector<Address> SystemResolve(const std::string& host);
bool IsAddressLiteral(const std::string& host);
bool ProxyConfigured();

}
