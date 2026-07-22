#include "tray/TrayApp.h"

#include "common/Config.h"
#include "common/Log.h"

#include <fstream>
#include <iterator>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace remote_assist {

namespace {

constexpr UINT kCallbackMessage = WM_USER + 1;
constexpr UINT kMenuCommandShowPw = 1001;
constexpr UINT kMenuCommandAbout = 1002;
constexpr UINT kMenuCommandExit = 1003;
constexpr const wchar_t* kClassName = L"RemoteAssistTrayWindow";

// 读取并删除 .initial-password 文件(由 service 首次生成密码时写入)。
std::wstring ReadAndDeleteInitialPassword() {
    const std::wstring path = ConfigDir() + L"\\.initial-password";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    const std::string s((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();
    DeleteFileW(path.c_str());
    return std::wstring(s.begin(), s.end());
}

}  // namespace

TrayApp::~TrayApp() {
    RemoveIcon();
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kCallbackMessage) {
        auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && lp == WM_RBUTTONUP) {
            self->ShowMenu();
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) {
            return 0;
        }
        switch (LOWORD(wp)) {
            case kMenuCommandShowPw:
                self->ShowPasswordDialog();
                break;
            case kMenuCommandExit:
                DestroyWindow(hwnd);
                break;
            default:
                break;
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayApp::CreateIcon() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayApp::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, kClassName, L"RemoteAssist Tray", 0,
                            0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kCallbackMessage;
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid_.szTip, L"RemoteAssist");
    Shell_NotifyIconW(NIM_ADD, &nid_);

    hMenu_ = CreatePopupMenu();
    AppendMenuW(hMenu_, MF_STRING, kMenuCommandShowPw, L"显示访问密码");
    AppendMenuW(hMenu_, MF_STRING, kMenuCommandAbout, L"关于 RemoteAssist");
    AppendMenuW(hMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu_, MF_STRING, kMenuCommandExit, L"退出托盘");
}

void TrayApp::ShowMenu() {
    if (!hMenu_) {
        return;
    }
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(hMenu_, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
}

void TrayApp::ShowPasswordDialog() {
    std::wstring msg;
    if (password_.empty()) {
        msg = L"密码已展示过或非首次启动。如需重置,请删除当前 exe 同目录的 config.json 后重启服务。";
    } else {
        msg = L"访问密码(仅本次显示,请妥善保存):\n\n" + password_ +
              L"\n\n浏览器打开 http://<本机IP>:7980/ 后输入此密码连接。";
    }
    MessageBoxW(hwnd_, msg.c_str(), L"RemoteAssist 访问密码", MB_OK | MB_ICONINFORMATION);
}

void TrayApp::RemoveIcon() {
    if (nid_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        nid_.hWnd = nullptr;
    }
    if (hMenu_) {
        DestroyMenu(hMenu_);
        hMenu_ = nullptr;
    }
}

int TrayApp::Run() {
    log::Init(LogDir());
    log::Info("tray starting");

    password_ = ReadAndDeleteInitialPassword();

    CreateIcon();
    if (!password_.empty()) {
        // 首次启动弹一次密码,降低用户漏看的概率。
        ShowPasswordDialog();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveIcon();
    log::Info("tray exit");
    return 0;
}

}  // namespace remote_assist
