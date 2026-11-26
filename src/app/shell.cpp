#include "app/shell.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <deque>

#include "core/installation.h"
#include "core/instance_folders.h"
#include "core/util.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace rwnd::app {

namespace {
constexpr UINT WM_APP_INVOKE = WM_APP + 1;
constexpr wchar_t ClassName[] = L"RWND.Parabellum.Shell";
constexpr int DefaultWidth = 1320, DefaultHeight = 820, MinWidth = 1160, MinHeight = 720;

Shell* g_shell = nullptr;
std::mutex g_queueMutex;
std::deque<std::function<void()>> g_queue;
HWND g_target = nullptr;

void FlushQueue() {
    std::deque<std::function<void()>> work;
    {
        std::lock_guard lock(g_queueMutex);
        work.swap(g_queue);
    }
    for (auto& f : work) f();
}

std::wstring MimeType(std::string_view name) {
    auto ext = ToLowerAscii(name.substr(name.rfind('.') == std::string_view::npos ? name.size() : name.rfind('.')));
    if (ext == ".html") return L"text/html; charset=utf-8";
    if (ext == ".css") return L"text/css; charset=utf-8";
    if (ext == ".js") return L"text/javascript; charset=utf-8";
    if (ext == ".ttf") return L"font/ttf";
    if (ext == ".svg") return L"image/svg+xml";
    if (ext == ".png") return L"image/png";
    if (ext == ".json") return L"application/json";
    return L"application/octet-stream";
}

int Scale(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

#ifdef NDEBUG
constexpr bool Development = false;
#else
constexpr bool Development = true;
#endif
}

void PostToUi(std::function<void()> work) {
    HWND target;
    {
        std::lock_guard lock(g_queueMutex);
        g_queue.push_back(std::move(work));
        target = g_target;
    }
    if (target) PostMessageW(target, WM_APP_INVOKE, 0, 0);
}

Shell* Shell::Instance() {
    return g_shell;
}

Shell::Shell(StartMode mode, bool startHidden) : mode_(mode), startHidden_(startHidden) {
    g_shell = this;
    web_ = Pak::FromResource(L"WEB");
}

Shell::~Shell() {
    if (webview_) webview_->Release();
    if (controller_) controller_->Release();
    if (environment_) environment_->Release();
    g_shell = nullptr;
}

void Shell::Create(std::function<void()> onReady) {
    onReady_ = std::move(onReady);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0x0B, 0x0D, 0x10));
    wc.lpszClassName = ClassName;
    RegisterClassExW(&wc);

    POINT origin{0, 0};
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(monitor, &mi);
    UINT dpi = 96;
    {
        HDC dc = GetDC(nullptr);
        dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ReleaseDC(nullptr, dc);
        UINT x, y;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (auto shcore = LoadLibraryW(L"shcore.dll"))
            if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
                fn && SUCCEEDED(fn(monitor, 0, &x, &y)))
                dpi = x;
    }
    if (mode_ == StartMode::Preview) dpi = 96;
    int width = Scale(DefaultWidth, dpi), height = Scale(DefaultHeight, dpi);
    RECT work = mi.rcWork;
    int x = work.left + ((work.right - work.left) - width) / 2, y = work.top + ((work.bottom - work.top) - height) / 2;
    std::wstring title = mode_ == StartMode::Setup ? L"Install RWND Parabellum" : L"RWND Parabellum";
    DWORD exStyle = mode_ == StartMode::Preview ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0;
    hwnd_ = CreateWindowExW(exStyle, ClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width,
                            height, nullptr, nullptr, instance, this);
    if (!hwnd_) ThrowLastError("Cannot create the launcher window");
    {
        std::lock_guard lock(g_queueMutex);
        g_target = hwnd_;
    }
    if (!g_queue.empty()) PostMessageW(hwnd_, WM_APP_INVOKE, 0, 0);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
    int round = 2;
    DwmSetWindowAttribute(hwnd_, 33, &round, sizeof round);
    MARGINS shadow{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &shadow);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (mode_ == StartMode::Preview)
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    else if (!startHidden_)
        ShowWindow(hwnd_, SW_SHOWNORMAL);
    InitWebView();
}

void Shell::InitWebView() {
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    std::wstring arguments = L"--disable-features=msSmartScreenProtection";
    if (mode_ == StartMode::Preview) arguments += L",CalculateNativeWinOcclusion --force-device-scale-factor=1";
    options->put_AdditionalBrowserArguments(arguments.c_str());
    std::wstring dataFolder =
        mode_ == StartMode::Preview
            ? (fs::path(TempDirectory()) / (L"rwnd-preview-webview2-" + Widen(Guid::New().N()))).wstring()
            : (fs::path(InstanceFolders::DataDirectory()) / L"WebView2").wstring();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataFolder.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT result,
                                                                                    ICoreWebView2Environment* env)
                                                                                 -> HRESULT {
            if (FAILED(result) || !env) {
                MessageBoxW(hwnd_,
                            L"RWND Parabellum could not start Microsoft Edge WebView2. Restart your PC and try again.",
                            L"RWND Parabellum", MB_ICONERROR);
                Destroy();
                return S_OK;
            }
            environment_ = env;
            env->AddRef();
            return env->CreateCoreWebView2Controller(
                hwnd_,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                        if (FAILED(result) || !controller) {
                            MessageBoxW(hwnd_,
                                        L"RWND Parabellum could not open its window. Restart your PC and try again.",
                                        L"RWND Parabellum", MB_ICONERROR);
                            Destroy();
                            return S_OK;
                        }
                        controller_ = controller;
                        controller->AddRef();
                        controller->get_CoreWebView2(&webview_);
                        OnWebViewReady();
                        return S_OK;
                    })
                    .Get());
        }).Get());
    if (FAILED(hr)) {
        MessageBoxW(hwnd_, L"RWND Parabellum could not start Microsoft Edge WebView2.", L"RWND Parabellum",
                    MB_ICONERROR);
        Destroy();
    }
}

void Shell::OnWebViewReady() {
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller_->QueryInterface(IID_PPV_ARGS(&controller2))))
        controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0x0B, 0x0D, 0x10});
    if (mode_ == StartMode::Preview) {
        ComPtr<ICoreWebView2Controller3> controller3;
        if (SUCCEEDED(controller_->QueryInterface(IID_PPV_ARGS(&controller3)))) {
            controller3->put_ShouldDetectMonitorScaleChanges(FALSE);
            controller3->put_RasterizationScale(1.0);
        }
    }

    ComPtr<ICoreWebView2Settings> settings;
    webview_->get_Settings(&settings);
    settings->put_AreDevToolsEnabled(Development);
    settings->put_AreDefaultContextMenusEnabled(Development);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    settings->put_IsBuiltInErrorPageEnabled(FALSE);
    if (ComPtr<ICoreWebView2Settings3> s3; SUCCEEDED(settings.As(&s3)))
        s3->put_AreBrowserAcceleratorKeysEnabled(Development);
    if (ComPtr<ICoreWebView2Settings4> s4; SUCCEEDED(settings.As(&s4))) {
        s4->put_IsPasswordAutosaveEnabled(FALSE);
        s4->put_IsGeneralAutofillEnabled(FALSE);
    }
    if (ComPtr<ICoreWebView2Settings5> s5; SUCCEEDED(settings.As(&s5))) s5->put_IsPinchZoomEnabled(FALSE);
    if (ComPtr<ICoreWebView2Settings6> s6; SUCCEEDED(settings.As(&s6))) s6->put_IsSwipeNavigationEnabled(FALSE);
    if (ComPtr<ICoreWebView2Settings9> s9; SUCCEEDED(settings.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);

    if (ComPtr<ICoreWebView2_3> w3; SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&w3)))) {
        fs::path userMedia = fs::path(InstanceFolders::DataDirectory()) / L"Media";
        w3->SetVirtualHostNameToFolderMapping(UserMediaHost, userMedia.c_str(),
                                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }

    std::wstring filter = std::wstring(AppOrigin) + L"*";
    webview_->AddWebResourceRequestedFilter(filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    std::wstring mediaFilter = L"https://" + std::wstring(MediaHost) + L"/*";
    webview_->AddWebResourceRequestedFilter(mediaFilter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_MEDIA);
    EventRegistrationToken token;
    webview_->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>([this](ICoreWebView2*,
                                                                       ICoreWebView2WebResourceRequestedEventArgs* args)
                                                                    -> HRESULT {
            ServeResource(args);
            return S_OK;
        }).Get(),
        &token);

    webview_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                         [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                             LPWSTR uri = nullptr;
                                             args->get_Uri(&uri);
                                             std::wstring u = uri ? uri : L"";
                                             CoTaskMemFree(uri);
                                             if (!IStartsWithW(u, AppOrigin)) args->put_Cancel(TRUE);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &token);
    webview_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                         [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                             args->put_Handled(TRUE);
                                             LPWSTR uri = nullptr;
                                             args->get_Uri(&uri);
                                             std::wstring u = uri ? uri : L"";
                                             CoTaskMemFree(uri);
                                             if (IStartsWithW(u, L"https://"))
                                                 ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr,
                                                               SW_SHOWNORMAL);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &token);
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>([this](ICoreWebView2*,
                                                                     ICoreWebView2WebMessageReceivedEventArgs* args)
                                                                  -> HRESULT {
            LPWSTR source = nullptr;
            args->get_Source(&source);
            bool trusted = source && IStartsWithW(source, AppOrigin);
            CoTaskMemFree(source);
            if (!trusted) return S_OK;
            LPWSTR json = nullptr;
            if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) HandleMessage(json);
            CoTaskMemFree(json);
            return S_OK;
        }).Get(),
        &token);
    for (auto& script : scripts_) webview_->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr);
    webview_->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>([this](ICoreWebView2*,
                                                                      ICoreWebView2NavigationCompletedEventArgs*)
                                                                   -> HRESULT {
            for (auto& script : scripts_) webview_->ExecuteScript(script.c_str(), nullptr);
            if (!pageReady_) {
                pageReady_ = true;
                if (onReady_) onReady_();
            }
            return S_OK;
        }).Get(),
        &token);
    webview_->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>([this](ICoreWebView2*,
                                                                ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind;
            args->get_ProcessFailedKind(&kind);
            if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
                webview_->Reload();
            else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                MessageBoxW(hwnd_, L"RWND Parabellum's window stopped unexpectedly. Open RWND again.",
                            L"RWND Parabellum", MB_ICONERROR);
                Destroy();
            }
            return S_OK;
        }).Get(),
        &token);

    Resize();
    controller_->put_IsVisible(TRUE);
    std::wstring start = std::wstring(AppOrigin) + L"index.html";
    if (mode_ == StartMode::Setup)
        start += L"?mode=setup";
    else if (mode_ == StartMode::Preview)
        start += L"?mode=preview";
    webview_->Navigate(start.c_str());
}

void Shell::ServeResource(void* raw) {
    auto args = static_cast<ICoreWebView2WebResourceRequestedEventArgs*>(raw);
    ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    LPWSTR uri = nullptr;
    request->get_Uri(&uri);
    std::wstring u = uri ? uri : L"";
    CoTaskMemFree(uri);
    if (std::wstring media = L"https://" + std::wstring(MediaHost) + L"/"; IStartsWithW(u, media)) {
        ServeMedia(args, request.Get(), u.substr(media.size()));
        return;
    }
    if (!IStartsWithW(u, AppOrigin)) return;
    std::string path = Narrow(u.substr(wcslen(AppOrigin)));
    path = path.substr(0, path.find_first_of("?#"));
    if (path.empty()) path = "index.html";
    const PakEntry* entry = web_.Find(path);
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (!entry) {
        environment_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
    } else {
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(entry->bytes.data(), static_cast<UINT>(entry->bytes.size())));
        std::wstring headers =
            L"Content-Type: " + MimeType(path) +
            L"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
            L"Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; font-src 'self'; "
            L"img-src 'self' data: https:; media-src 'self' https://media.rwnd.invalid https://usermedia.rwnd.invalid https:; connect-src 'self'; "
            L"frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'";
        environment_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
    }
    args->put_Response(response.Get());
}

void Shell::ServeMedia(void* rawArgs, void* rawRequest, const std::wstring& escapedName) {
    auto args = static_cast<ICoreWebView2WebResourceRequestedEventArgs*>(rawArgs);
    auto request = static_cast<ICoreWebView2WebResourceRequest*>(rawRequest);
    std::wstring name(escapedName.substr(0, escapedName.find_first_of(L"?#")));
    DWORD length = static_cast<DWORD>(name.size() + 1);
    std::wstring unescaped(length, L'\0');
    if (SUCCEEDED(UrlUnescapeW(name.data(), unescaped.data(), &length, 0))) name = unescaped.c_str();
    std::optional<std::span<const uint8_t>> bytes;
    if (!name.empty() && name.find_first_of(L"/\\") == std::wstring::npos)
        bytes = payload::Find("Media/" + Narrow(name));
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (!bytes || bytes->empty()) {
        environment_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
        args->put_Response(response.Get());
        return;
    }
    const uint64_t total = bytes->size(), chunk = 4ull * 1024 * 1024;
    uint64_t start = 0, end = total - 1;
    bool ranged = false;
    ComPtr<ICoreWebView2HttpRequestHeaders> headers;
    LPWSTR range = nullptr;
    if (SUCCEEDED(request->get_Headers(&headers)) && SUCCEEDED(headers->GetHeader(L"Range", &range)) && range) {
        unsigned long long a = 0, b = 0;
        int read = swscanf_s(range, L"bytes=%llu-%llu", &a, &b);
        if (read >= 1 && a < total) {
            ranged = true;
            start = a;
            end = read == 2 && b >= a ? std::min<uint64_t>(b, total - 1) : total - 1;
        }
        CoTaskMemFree(range);
    }
    if (ranged) end = std::min(end, start + chunk - 1);
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes->data() + start, static_cast<UINT>(end - start + 1)));
    std::wstring lower = ToLowerW(name);
    std::wstring type =
        lower.ends_with(L".mp4") || lower.ends_with(L".m4v") ? L"video/mp4" : L"application/octet-stream";
    std::wstring head = L"Content-Type: " + type +
                        L"\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                        L"Content-Length: " +
                        std::to_wstring(end - start + 1);
    if (ranged)
        head += L"\r\nContent-Range: bytes " + std::to_wstring(start) + L"-" + std::to_wstring(end) + L"/" +
                std::to_wstring(total);
    environment_->CreateWebResourceResponse(stream.Get(), ranged ? 206 : 200, ranged ? L"Partial Content" : L"OK",
                                            head.c_str(), &response);
    args->put_Response(response.Get());
}

void Shell::On(const std::string& method, RpcHandler handler, bool uiThread) {
    handlers_[method] = {std::move(handler), uiThread};
}

void Shell::PostJson(const Json& message) {
    if (!webview_) return;
    std::wstring text = Widen(ToJson(message));
    webview_->PostWebMessageAsJson(text.c_str());
}

void Shell::Emit(const std::string& event, const Json& data) {
    Json message = {{"event", event}, {"data", data}};
    PostToUi([this, message] { PostJson(message); });
}

void Shell::Reply(uint64_t id, bool ok, const Json& payload) {
    Json message = {{"id", id}, {"ok", ok}, {ok ? "result" : "error", payload}};
    PostToUi([this, message] {
        {
            std::lock_guard lock(callsMutex_);
            calls_.erase(message["id"].get<uint64_t>());
        }
        PostJson(message);
    });
}

static Json ErrorJson(const Error& e) {
    static const char* kinds[] = {"io",       "invalidData",   "invalidOperation", "unauthorizedAccess", "http",
                                  "canceled", "loginRequired", "argument",         "steamDownload",      "timeout",
                                  "win32"};
    return {{"kind", kinds[static_cast<int>(e.kind)]}, {"message", e.what()}, {"status", e.status}};
}

void Shell::HandleMessage(const std::wstring& text) {
    Json message;
    try {
        message = ParseJson(Narrow(text));
    } catch (const Error&) {
        return;
    }
    if (!message.is_object()) return;
    if (auto cancel = GetInt(message, "cancel")) {
        std::shared_ptr<CancelSource> source;
        {
            std::lock_guard lock(callsMutex_);
            auto it = calls_.find(static_cast<uint64_t>(*cancel));
            if (it != calls_.end()) source = it->second;
        }
        if (source) source->Cancel();
        return;
    }
    auto id = GetInt(message, "id");
    auto method = GetString(message, "method");
    if (!id || !method) return;
    Json params = message.contains("params") ? message["params"] : Json::object();
    if (MayCall && !MayCall(*method)) {
        Reply(static_cast<uint64_t>(*id), false,
              {{"kind", "invalidOperation"}, {"message", "Accept the Terms of Service first."}, {"status", 0}});
        return;
    }
    auto it = handlers_.find(*method);
    if (it == handlers_.end()) {
        Reply(static_cast<uint64_t>(*id), false,
              {{"kind", "argument"}, {"message", "Unknown call " + *method}, {"status", 0}});
        return;
    }
    auto source = std::make_shared<CancelSource>(closing);
    {
        std::lock_guard lock(callsMutex_);
        calls_[static_cast<uint64_t>(*id)] = source;
    }
    auto run = [this, fn = it->second.fn, params, source, callId = static_cast<uint64_t>(*id)] {
        try {
            Json result = fn(params, source->Token());
            Reply(callId, true, result);
        } catch (const Error& e) {
            Reply(callId, false, ErrorJson(e));
        } catch (const std::exception& e) {
            Reply(callId, false, {{"kind", "invalidOperation"}, {"message", e.what()}, {"status", 0}});
        }
    };
    if (it->second.ui)
        PostToUi(std::move(run));
    else {
        auto work = new std::function<void()>(std::move(run));
        if (!TrySubmitThreadpoolCallback(
                [](PTP_CALLBACK_INSTANCE, void* context) {
                    std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(context));
                    (*f)();
                },
                work, nullptr)) {
            std::unique_ptr<std::function<void()>> f(work);
            (*f)();
        }
    }
}

void Shell::Resize() {
    if (!controller_) return;
    RECT bounds;
    GetClientRect(hwnd_, &bounds);
    controller_->put_Bounds(bounds);
}

void Shell::ShowFromTray() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    Emit("shell.shown");
}

void Shell::HideToTray() {
    ShowWindow(hwnd_, SW_HIDE);
}
bool Shell::IsVisible() const {
    return IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}
bool Shell::IsActive() const {
    return GetForegroundWindow() == hwnd_;
}
void Shell::SetTitle(const std::wstring& title) {
    SetWindowTextW(hwnd_, title.c_str());
}
void Shell::Close() {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}
void Shell::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void Shell::ExecuteScript(const std::wstring& script) {
    if (webview_) webview_->ExecuteScript(script.c_str(), nullptr);
}

void Shell::AddScript(const std::wstring& script) {
    scripts_.push_back(script);
    if (!webview_) return;
    webview_->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr);
    if (pageReady_) webview_->ExecuteScript(script.c_str(), nullptr);
}

void Shell::CapturePng(const std::wstring& file, std::function<void(bool)> done) {
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(file.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr,
                                      &stream))) {
        done(false);
        return;
    }
    webview_->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([stream, done](HRESULT result) -> HRESULT {
            stream->Commit(STGC_DEFAULT);
            done(SUCCEEDED(result));
            return S_OK;
        }).Get());
}

LRESULT CALLBACK Shell::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Shell* self = reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Shell*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Shell::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_APP_INVOKE:
            FlushQueue();
            return 0;
        case WM_NCCALCSIZE:
            if (wParam) {
                auto params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                if (IsZoomed(hwnd_)) {
                    UINT dpi = GetDpiForWindow(hwnd_);
                    int frame =
                        GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    params->rgrc[0].left += frame;
                    params->rgrc[0].right -= frame;
                    params->rgrc[0].top += frame;
                    params->rgrc[0].bottom -= frame;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd_, message, wParam, lParam);
            return hit == HTCAPTION || hit == HTSYSMENU ? HTCLIENT : hit;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lParam);
            UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
            info->ptMinTrackSize = {Scale(MinWidth, dpi), Scale(MinHeight, dpi)};
            return 0;
        }
        case WM_DPICHANGED: {
            if (mode_ == StartMode::Preview) return 0;
            auto suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SIZE:
            Resize();
            if (pageReady_) Emit("window.state", {{"maximized", IsZoomed(hwnd_) != FALSE}});
            return 0;
        case WM_MOVE:
        case WM_MOVING:
            if (controller_) controller_->NotifyParentWindowPositionChanged();
            break;
        case WM_ACTIVATE:
            if (pageReady_) Emit("window.active", {{"active", LOWORD(wParam) != WA_INACTIVE}});
            break;
        case WM_SETFOCUS:
            if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_CLOSE:
            if (OnCloseRequested && !OnCloseRequested()) return 0;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY: {
            std::lock_guard lock(g_queueMutex);
            g_target = nullptr;
        }
            if (controller_) controller_->Close();
            if (OnDestroyed) OnDestroyed();
            PostQuitMessage(0);
            return 0;
        default:
            if (message >= WM_APP + 2 && message < 0xBFFF && OnAppMessage) {
                OnAppMessage(message, wParam, lParam);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}
