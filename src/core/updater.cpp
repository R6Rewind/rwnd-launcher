#include "core/updater.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>

#include "core/installation.h"
#include "core/json.h"
#include "core/settings.h"

namespace rwnd::updater {

bool IsAllowed(const std::string& url) {
    auto u = Url::Parse(url);
    return u && (u->IsHttps() || (u->scheme == "http" && u->IsLoopback()));
}

std::optional<std::string> ManifestUrl() {
    auto configured = settings::Endpoint(L"RWND_UPDATE_URL");
    if (!configured) configured = settings::String("updateManifestUrl");
    return configured && IsAllowed(*configured) ? configured : std::nullopt;
}

bool VerifySignature(std::span<const uint8_t> data, std::span<const uint8_t> signature, std::span<const uint8_t> spki) {
    static const uint8_t prefix[] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06,
                                     0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04};
    if (spki.size() != sizeof prefix + 64 || std::memcmp(spki.data(), prefix, sizeof prefix) != 0 ||
        signature.size() != 64)
        return false;
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), spki.data() + sizeof prefix, 64);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), static_cast<ULONG>(blob.size()),
                            0) >= 0) {
        Bytes hash = Sha256Of(data);
        ok = BCryptVerifySignature(key, nullptr, hash.data(), static_cast<ULONG>(hash.size()),
                                   const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0) >= 0;
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

ReleaseInfo Verify(const std::string& latestJson, const std::string& publicKeyBase64) {
    Json signedJson = ParseJson(latestJson);
    Require(signedJson.is_object(), "Empty update manifest.");
    Bytes manifest = Base64Decode(RequireString(signedJson, "manifest"));
    Bytes signature = Base64Decode(RequireString(signedJson, "signature"));
    if (!VerifySignature(manifest, signature, Base64Decode(publicKeyBase64)))
        Throw(ErrorKind::InvalidData, "The update manifest signature is invalid.");
    Json m = ParseJson(manifest);
    Require(m.is_object(), "Invalid update manifest.");
    ReleaseInfo release;
    try {
        release.product = GetString(m, "product").value_or("");
        release.version = GetString(m, "version").value_or("");
        release.url = GetString(m, "url").value_or("");
        release.sha256 = GetString(m, "sha256").value_or("");
        release.size = GetInt(m, "size").value_or(0);
        release.notes = GetString(m, "notes");
        release.published = GetTicks(m, "published").value_or(0);
    } catch (const Error&) {
        Throw(ErrorKind::InvalidData, "Invalid update manifest.");
    }
    if (release.product != "RWND.Parabellum" || !Version::Parse(release.version) || release.size <= 0 ||
        release.size > MaxSize || release.sha256.size() != 64 || !IsAllowed(release.url))
        Throw(ErrorKind::InvalidData, "The update manifest is invalid.");
    return release;
}

std::optional<ReleaseInfo> Check(const CancelToken& cancel, std::shared_ptr<HttpHandler> http,
                                 const std::string& publicKeyBase64) {
    auto url = ManifestUrl();
    if (!url) return std::nullopt;
    if (!http) http = WinHttp();
    HttpRequest request;
    request.url = *url;
    request.followRedirects = true;
    request.timeoutMs = 10000;
    request.noCache = true;
    HttpResponse response = http->Send(request, cancel);
    if (!response.IsSuccess()) ThrowStatus(response.status);
    ReleaseInfo release = Verify(response.body, publicKeyBase64);
    return Version::Parse(release.version)->Compare(LauncherVersion()) > 0 ? std::optional(release) : std::nullopt;
}

bool SendsSignIn(const std::string& url) {
    auto u = Url::Parse(url);
    auto site = Url::Parse(settings::String("websiteUrl").value_or(""));
    return u && site && u->IsHttps() && site->IsHttps() && u->Origin() == site->Origin();
}

fs::path Download(const ReleaseInfo& release, const std::function<void(double)>& progress, const CancelToken& cancel,
                  std::shared_ptr<HttpHandler> http, const std::string& bearer) {
    Installation::EnsureWritable();
    if (!http) http = WinHttp();
    fs::path target = Installation::InstalledExe().wstring() + L".download";
    HttpRequest request;
    request.url = release.url;
    request.followRedirects = bearer.empty();
    if (!bearer.empty()) request.headers.push_back({"Authorization", "Bearer " + bearer});
    request.timeoutMs = 15 * 60 * 1000;
    Sha256 hash;
    int64_t total = 0;
    std::unique_ptr<File> output;
    try {
        http->Stream(
            request,
            [&](const HttpResponse& response) {
                if (response.status == 401) Throw(ErrorKind::Http, "sign in to download the update", 401);
                if (response.status == 403)
                    Throw(ErrorKind::Http, "this account needs an invite: redeem your code at rwnd.gg/download", 403);
                if (!response.IsSuccess()) ThrowStatus(response.status);
                if (!IsAllowed(response.finalUrl))
                    Throw(ErrorKind::InvalidData, "The update download was redirected to an insecure address.");
                output = std::make_unique<File>(target, File::Mode::Create);
            },
            [&](const uint8_t* data, size_t size) {
                total += static_cast<int64_t>(size);
                if (total > release.size) Throw(ErrorKind::InvalidData, "The update download is larger than expected.");
                hash.Update(data, size);
                output->Write(data, size);
                progress(static_cast<double>(total) / static_cast<double>(release.size));
            },
            cancel);
        if (output) output->Close();
        if (total != release.size || hash.FinalHex() != ToLowerAscii(release.sha256)) {
            DeleteFileIfExists(target);
            Throw(ErrorKind::InvalidData, "The update download failed verification. Try again later.");
        }
    } catch (...) {
        output.reset();
        try {
            if (total != release.size) DeleteFileIfExists(target);
        } catch (...) {
        }
        throw;
    }
    return target;
}

void ApplyAndRestart(const fs::path& verified, bool stayInTray) {
    fs::path exe = Installation::InstalledExe();
    fs::path old = exe.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".old";
    std::vector<std::wstring> arguments = {L"--after", std::to_wstring(GetCurrentProcessId())};
    if (stayInTray) arguments.push_back(L"--tray");
    MoveNoReplace(exe, old);
    try {
        MoveNoReplace(verified, exe);
    } catch (...) {
        MoveNoReplace(old, exe);
        throw;
    }
    std::wstring command = QuoteArgument(exe.wstring()) + L" " + JoinArguments(arguments);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring directory = ExeDirectory().wstring();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &si, &pi))
        ThrowLastError("Cannot start the new launcher");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

}
