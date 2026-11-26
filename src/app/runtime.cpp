#include "app/runtime.h"

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <ole2.h>

#include <WebView2.h>

#include "core/http.h"
#include "core/process.h"
#include "core/util.h"

#pragma comment(lib, "wintrust.lib")

namespace rwnd::app {

namespace {

bool RuntimePresent() {
    LPWSTR version = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    bool ok = SUCCEEDED(hr) && version && *version;
    if (version) CoTaskMemFree(version);
    return ok;
}

bool SignedByMicrosoft(const fs::path& file) {
    WINTRUST_FILE_INFO fileInfo{sizeof fileInfo};
    fileInfo.pcwszFilePath = file.c_str();
    WINTRUST_DATA trust{sizeof trust};
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &fileInfo;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &trust);
    bool microsoft = false;
    if (status == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        if (cert && cert->pCert) {
            wchar_t name[256]{};
            CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            microsoft = std::wstring(name) == L"Microsoft Corporation";
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    return microsoft;
}

}

bool EnsureWebView2Runtime(bool quiet) {
    if (RuntimePresent()) return true;
    if (quiet) return false;
    int choice =
        MessageBoxW(nullptr,
                    L"RWND Parabellum needs the Microsoft Edge WebView2 Runtime, which Windows normally includes.\n\n"
                    L"Install it now? Windows downloads it from Microsoft (about 2 MB to start).",
                    L"RWND Parabellum", MB_YESNO | MB_ICONINFORMATION);
    if (choice != IDYES) {
        process::Open(L"https://developer.microsoft.com/microsoft-edge/webview2/");
        return false;
    }
    fs::path setup = fs::path(TempDirectory()) / (L"MicrosoftEdgeWebview2Setup-" + Widen(Guid::New().N()) + L".exe");
    try {
        HttpRequest request;
        request.url = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
        request.followRedirects = true;
        request.timeoutMs = 5 * 60 * 1000;
        HttpResponse response = WinHttp()->Send(request, CancelToken::None());
        if (!response.IsSuccess() || response.body.size() < 1024)
            Throw(ErrorKind::Http, "The download did not complete.");
        WriteAllText(setup, response.body);
        if (!SignedByMicrosoft(setup)) Throw(ErrorKind::InvalidData, "The download is not signed by Microsoft.");
        int code = process::RunHidden(setup, {L"/silent", L"/install"});
        DeleteFileIfExists(setup);
        if (RuntimePresent()) return true;
        Throw(ErrorKind::InvalidOperation, "The installer finished with code " + std::to_string(code) + ".");
    } catch (const std::exception& e) {
        try {
            DeleteFileIfExists(setup);
        } catch (...) {
        }
        MessageBoxW(
            nullptr,
            (L"The WebView2 Runtime could not be installed (" + Widen(e.what()) +
             L").\n\nInstall the \"Evergreen Bootstrapper\" from Microsoft's WebView2 page, then open RWND Parabellum again.")
                .c_str(),
            L"RWND Parabellum", MB_ICONERROR);
        process::Open(L"https://developer.microsoft.com/microsoft-edge/webview2/");
        return false;
    }
}

}
