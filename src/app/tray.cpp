#include "app/tray.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

namespace rwnd::app {

namespace {
constexpr UINT WM_TRAY = WM_APP + 20;
constexpr UINT IconId = 1;
enum Item : UINT { Play = 100, Open, Separator, SignOut, Quit };
constexpr COLORREF Panel = RGB(0x17, 0x1A, 0x1F), Hover = RGB(0x2E, 0x33, 0x3B), Edge = RGB(0x3A, 0x3F, 0x47),
                   Line = RGB(0x2A, 0x2E, 0x35);
constexpr COLORREF TextOn = RGB(0xF2, 0xEE, 0xE6), TextOff = RGB(0x9A, 0x9E, 0xA6);
}

Tray::Tray() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"RWND.Parabellum.Tray";
    RegisterClassExW(&wc);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"RWND Parabellum tray", WS_POPUP, 0, 0, 0, 0, nullptr,
                            nullptr, instance, this);
    if (hwnd_) {
        ChangeWindowMessageFilterEx(hwnd_, WM_TRAY, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd_, WM_COMMAND, MSGFLT_ALLOW, nullptr);
    }
    icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON), 0));
    HDC dc = GetDC(nullptr);
    int height = -MulDiv(95, GetDeviceCaps(dc, LOGPIXELSY), 720);
    ReleaseDC(nullptr, dc);
    font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    bold_ = CreateFontW(height, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    Add();
}

Tray::~Tray() {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = hwnd_;
    data.uID = IconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (hwnd_) DestroyWindow(hwnd_);
    if (icon_) DestroyIcon(icon_);
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
}

void Tray::Add() {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = hwnd_;
    data.uID = IconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_TRAY;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, L"RWND Parabellum");
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void Tray::Update(const std::wstring* account, const std::wstring& playLabel, bool canPlay) {
    playLabel_ = playLabel;
    canPlay_ = canPlay;
    signOutLabel_ = account ? L"Log out of " + *account : L"Log out";
    canSignOut_ = account != nullptr;
}

void Tray::Notify(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW data{sizeof data};
    data.hWnd = hwnd_;
    data.uID = IconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_NONE;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_OWNERDRAW | (canPlay_ ? 0 : MF_GRAYED), Play, reinterpret_cast<LPCWSTR>(Play));
    AppendMenuW(menu, MF_OWNERDRAW, Open, reinterpret_cast<LPCWSTR>(Open));
    AppendMenuW(menu, MF_OWNERDRAW | MF_DISABLED, Separator, reinterpret_cast<LPCWSTR>(Separator));
    AppendMenuW(menu, MF_OWNERDRAW | (canSignOut_ ? 0 : MF_GRAYED), SignOut, reinterpret_cast<LPCWSTR>(SignOut));
    AppendMenuW(menu, MF_OWNERDRAW, Quit, reinterpret_cast<LPCWSTR>(Quit));
    MENUINFO info{sizeof info};
    info.fMask = MIM_BACKGROUND;
    HBRUSH background = CreateSolidBrush(Panel);
    info.hbrBack = background;
    SetMenuInfo(menu, &info);
    POINT at;
    GetCursorPos(&at);
    SetForegroundWindow(hwnd_);
    UINT chosen =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    DeleteObject(background);
    switch (chosen) {
        case Play:
            if (onPlay) onPlay();
            break;
        case Open:
            if (onOpen) onOpen();
            break;
        case SignOut:
            if (onSignOut) onSignOut();
            break;
        case Quit:
            if (onQuit) onQuit();
            break;
    }
}

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Tray* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Tray*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Tray::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreated_ && taskbarCreated_) {
        Add();
        return 0;
    }
    switch (message) {
        case WM_TRAY:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (onOpen) onOpen();
                    return 0;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    ShowMenu();
                    return 0;
                case NIN_BALLOONUSERCLICK:
                    if (onNotificationClicked) onNotificationClicked();
                    return 0;
            }
            return 0;
        case WM_MEASUREITEM: {
            auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item->itemID == Separator) {
                item->itemHeight = 9;
                item->itemWidth = 40;
                return TRUE;
            }
            HDC dc = GetDC(hwnd_);
            HGDIOBJ old = SelectObject(dc, item->itemID == Play ? bold_ : font_);
            std::wstring text = item->itemID == Play      ? playLabel_
                                : item->itemID == Open    ? L"Open RWND Parabellum"
                                : item->itemID == SignOut ? signOutLabel_
                                                          : L"Quit";
            SIZE size;
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            SelectObject(dc, old);
            ReleaseDC(hwnd_, dc);
            UINT dpi = GetDpiForWindow(hwnd_);
            int padX = MulDiv(6 + 8, dpi, 96), padY = MulDiv(5 + 2, dpi, 96);
            item->itemWidth = static_cast<UINT>(size.cx + padX * 2);
            item->itemHeight = static_cast<UINT>(size.cy + padY * 2);
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            HDC dc = item->hDC;
            RECT r = item->rcItem;
            HBRUSH panel = CreateSolidBrush(Panel);
            FillRect(dc, &r, panel);
            DeleteObject(panel);
            if (item->itemID == Separator) {
                HPEN pen = CreatePen(PS_SOLID, 1, Line);
                HGDIOBJ old = SelectObject(dc, pen);
                int y = (r.top + r.bottom) / 2;
                MoveToEx(dc, r.left + 4, y, nullptr);
                LineTo(dc, r.right - 4, y);
                SelectObject(dc, old);
                DeleteObject(pen);
                return TRUE;
            }
            bool enabled = !(item->itemState & (ODS_GRAYED | ODS_DISABLED));
            if ((item->itemState & ODS_SELECTED) && enabled) {
                RECT h{r.left + 2, r.top, r.right - 2, r.bottom};
                HBRUSH hover = CreateSolidBrush(Hover);
                FillRect(dc, &h, hover);
                DeleteObject(hover);
            }
            std::wstring text = item->itemID == Play      ? playLabel_
                                : item->itemID == Open    ? L"Open RWND Parabellum"
                                : item->itemID == SignOut ? signOutLabel_
                                                          : L"Quit";
            HGDIOBJ old = SelectObject(dc, item->itemID == Play ? bold_ : font_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, enabled ? TextOn : TextOff);
            RECT t = r;
            t.left += MulDiv(6 + 8, GetDpiForWindow(hwnd_), 96);
            DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &t,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
            SelectObject(dc, old);
            return TRUE;
        }
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}
