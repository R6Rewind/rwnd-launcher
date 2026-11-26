#pragma once
#include <functional>
#include <optional>
#include <string>

#include "core/http.h"

namespace rwnd {

struct ReleaseInfo {
    std::string product, version, url, sha256;
    int64_t size = 0;
    std::optional<std::string> notes;
    int64_t published = 0;
};

namespace updater {

inline constexpr const char* ReleasePublicKey =
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEWcJVAqBG9/p/Ji3Epzk8NGCl257jVqpFJ3ZDPXjDxjCr+2Ffq4z7CavIb7uHcbbIRyXgJmyv7HZ1sJxmp0zo8Q==";
inline constexpr int64_t MaxSize = 400LL * 1024 * 1024;

std::optional<std::string> ManifestUrl();
ReleaseInfo Verify(const std::string& latestJson, const std::string& publicKeyBase64 = ReleasePublicKey);
std::optional<ReleaseInfo> Check(const CancelToken& cancel, std::shared_ptr<HttpHandler> http = nullptr,
                                 const std::string& publicKeyBase64 = ReleasePublicKey);
fs::path Download(const ReleaseInfo& release, const std::function<void(double)>& progress, const CancelToken& cancel,
                  std::shared_ptr<HttpHandler> http = nullptr, const std::string& bearer = {});
bool SendsSignIn(const std::string& url);
void ApplyAndRestart(const fs::path& verified, bool stayInTray);
bool IsAllowed(const std::string& url);
bool VerifySignature(std::span<const uint8_t> data, std::span<const uint8_t> signature,
                     std::span<const uint8_t> subjectPublicKeyInfo);

}
}
