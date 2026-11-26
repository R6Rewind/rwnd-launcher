#pragma once
#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/json.h"
#include "core/pak.h"

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace rwnd::app {

enum class StartMode { Setup, Launcher, Preview };

void PostToUi(std::function<void()> work);

using RpcHandler = std::function<Json(const Json& params, const CancelToken& cancel)>;

class Shell {
public:
    Shell(StartMode mode, bool startHidden);
    ~Shell();

    void Create(std::function<void()> onReady = nullptr);
    HWND Hwnd() const { return hwnd_; }
    StartMode Mode() const { return mode_; }

    void On(const std::string& method, RpcHandler handler, bool uiThread = false);
    void Emit(const std::string& event, const Json& data = Json::object());

    void ShowFromTray();
    void HideToTray();
    bool IsVisible() const;
    bool IsActive() const;
    void SetTitle(const std::wstring& title);
    void Close();
    void Destroy();

    std::function<bool()> OnCloseRequested;
    std::function<void()> OnDestroyed;
    std::function<void(UINT, WPARAM, LPARAM)> OnAppMessage;
    std::function<bool(const std::string& method)> MayCall;

    void CapturePng(const std::wstring& file, std::function<void(bool)> done);
    void ExecuteScript(const std::wstring& script);
    void AddScript(const std::wstring& script);

    static Shell* Instance();
    CancelToken closing;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void InitWebView();
    void OnWebViewReady();
    void Resize();
    void HandleMessage(const std::wstring& json);
    void Reply(uint64_t id, bool ok, const Json& payload);
    void PostJson(const Json& message);
    void ServeResource(void* args);
    void ServeMedia(void* args, void* request, const std::wstring& escapedName);

    StartMode mode_;
    bool startHidden_;
    HWND hwnd_ = nullptr;
    std::function<void()> onReady_;
    ICoreWebView2Environment* environment_ = nullptr;
    ICoreWebView2Controller* controller_ = nullptr;
    ICoreWebView2* webview_ = nullptr;
    Pak web_;
    bool pageReady_ = false;
    std::vector<std::wstring> scripts_;
    struct Handler {
        RpcHandler fn;
        bool ui;
    };
    std::map<std::string, Handler> handlers_;
    std::mutex callsMutex_;
    std::map<uint64_t, std::shared_ptr<CancelSource>> calls_;
};

inline constexpr const wchar_t* AppOrigin = L"https://app.rwnd.invalid/";
inline constexpr const wchar_t* MediaHost = L"media.rwnd.invalid";
inline constexpr const wchar_t* UserMediaHost = L"usermedia.rwnd.invalid";

}
