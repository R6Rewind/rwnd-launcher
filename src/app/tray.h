#pragma once
#include <windows.h>

#include <functional>
#include <string>

namespace rwnd::app {

class Tray {
public:
    Tray();
    ~Tray();
    std::function<void()> onPlay, onOpen, onSignOut, onQuit, onNotificationClicked;
    void Update(const std::wstring* account, const std::wstring& playLabel, bool canPlay);
    void Notify(const std::wstring& title, const std::wstring& text);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void ShowMenu();
    void Add();

    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr;
    std::wstring playLabel_ = L"Play RWND", signOutLabel_ = L"Log out";
    bool canPlay_ = true, canSignOut_ = false;
    UINT taskbarCreated_ = 0;
};

}
