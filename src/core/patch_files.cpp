#include "core/patch_files.h"

#include <map>
#include <mutex>

#include "core/account_client.h"

namespace rwnd {

namespace {
constexpr uint64_t MaxSize = 128ull * 1024 * 1024;
std::mutex g_gate;
std::map<std::string, Bytes> g_cache;
std::shared_ptr<HttpHandler> g_http;
std::string g_api;
PatchFiles::TokenProvider g_token;
}

void PatchFiles::Configure(const std::string& apiBaseUrl, TokenProvider token, std::shared_ptr<HttpHandler> http) {
    std::lock_guard lock(g_gate);
    std::string base = apiBaseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    g_api = base + "/";
    g_token = std::move(token);
    g_http = http ? std::move(http) : WinHttp();
}

void PatchFiles::Reset() {
    std::lock_guard lock(g_gate);
    g_cache.clear();
    g_http.reset();
    g_api.clear();
    g_token = nullptr;
}

Bytes PatchFiles::Get(const std::string& sha256, const CancelToken& cancel) {
    if (!IsHexHash(sha256)) Throw(ErrorKind::Argument, "Invalid patch hash.");
    std::lock_guard lock(g_gate);
    if (auto it = g_cache.find(sha256); it != g_cache.end()) return it->second;
    if (!g_http || g_api.empty() || !g_token)
        Throw(ErrorKind::InvalidOperation, "Sign in to RWND to install the game files.");
    HttpRequest request;
    request.url = ResolveUrl(g_api, "game/patches/" + sha256);
    request.timeoutMs = 600000;
    request.headers.push_back({"Authorization", "Bearer " + g_token(cancel)});
    HttpResponse response = g_http->Send(request, cancel);
    if (response.status == 401 || response.status == 403)
        Throw(ErrorKind::Http, "Sign in to RWND again.", response.status);
    if (!response.IsSuccess())
        Throw(ErrorKind::InvalidOperation,
              ErrorMessage(response.body)
                  .value_or("The RWND patch service answered " + std::to_string(response.status) +
                            ". Try again in a moment."),
              response.status);
    if (response.contentLength && *response.contentLength > MaxSize)
        Throw(ErrorKind::InvalidData, "RWND patch is too large.");
    if (response.body.size() > MaxSize) Throw(ErrorKind::InvalidData, "RWND patch is too large.");
    Bytes bytes(response.body.begin(), response.body.end());
    if (Sha256Hex(bytes) != sha256) Throw(ErrorKind::InvalidData, "RWND patch checksum failed.");
    g_cache[sha256] = bytes;
    return bytes;
}

const std::vector<std::wstring>& PatchFiles::RetiredFiles() {
    static const std::vector<std::wstring> files = {
        L"launcher-settings.json",
        L"Patches\\parabellum-payload.json",
        L"ZstdSharp-LICENSE.txt",
    };
    return files;
}

int PatchFiles::RemoveRetired(const fs::path& directory) {
    int removed = 0;
    for (auto& relative : RetiredFiles())
        for (auto& name : {relative, relative + L".tmp"}) {
            fs::path file = directory / name;
            try {
                if (FileExists(file)) {
                    DeleteFileIfExists(file);
                    removed++;
                }
            } catch (const Error&) {
            }
        }
    return removed;
}

}
