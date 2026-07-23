#include "tray/TrayApp.h"

#include "common/Config.h"
#include "common/InitialPasswordChannel.h"
#include "common/Log.h"
#include "common/Path.h"
#include "common/RuntimeNames.h"

#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace remote_assist {

namespace {

constexpr UINT kCallbackMessage = WM_USER + 1;
constexpr UINT kMenuCommandOpenSetup = 1001;
constexpr UINT kMenuCommandShowPw = 1002;
constexpr UINT kMenuCommandAbout = 1003;
constexpr UINT kMenuCommandExit = 1004;
constexpr const wchar_t* kClassName = L"RemoteAssistTrayWindow";

// 从 Service 创建的短生命周期共享内存读取 UTF-8 密码。失败仅影响首次密码提示，
// 不阻止 Tray 图标和设置窗口正常启动。
std::wstring ReadInitialPassword(const std::wstring& channelName) {
    std::string utf8Password;
    if (!ReadInitialPasswordChannel(channelName, utf8Password)) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           utf8Password.data(),
                                           static_cast<int>(utf8Password.size()),
                                           nullptr, 0);
    if (length <= 0) {
        log::Warn("initial password channel is not valid UTF-8");
        return {};
    }
    std::wstring widePassword(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8Password.data(), static_cast<int>(utf8Password.size()),
                            widePassword.data(), length) != length) {
        log::Warn("initial password channel UTF-8 conversion failed");
        return {};
    }
    return widePassword;
}

}  // namespace

TrayApp::~TrayApp() {
    RemoveIcon();
    if (!password_.empty()) {
        SecureZeroMemory(password_.data(), password_.size() * sizeof(wchar_t));
        password_.clear();
    }
    if (instanceMutex_) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->taskbarCreatedMessage_ != 0 &&
        msg == self->taskbarCreatedMessage_) {
        // Explorer 重启会清空通知区域图标，但 Tray 进程本身仍存活；必须重新添加。
        self->AddIconToTaskbar();
        return 0;
    }
    if (msg == kCallbackMessage) {
        if (self) {
            // NOTIFYICON_VERSION_4 将通知消息放在 lParam 的低 16 位，高位携带
            // 图标 ID。直接比较完整 lParam 会导致 uID=1 时右键菜单永远不触发。
            const UINT eventMessage = LOWORD(static_cast<DWORD_PTR>(lp));
            if (eventMessage == WM_RBUTTONUP) {
                self->ShowMenu();
            } else if (eventMessage == WM_LBUTTONDBLCLK) {
                self->OpenSetupWindow();
            }
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        if (!self) {
            return 0;
        }
        switch (LOWORD(wp)) {
            case kMenuCommandOpenSetup:
                self->OpenSetupWindow();
                break;
            case kMenuCommandShowPw:
                self->ShowPasswordDialog();
                break;
            case kMenuCommandAbout:
                MessageBoxW(hwnd, L"RemoteAssist 局域网远程协助\n服务运行状态请在配置窗口查看。",
                            L"关于 RemoteAssist", MB_OK | MB_ICONINFORMATION);
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

bool TrayApp::AddIconToTaskbar() {
    if (!hwnd_) {
        return false;
    }
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        log::Error("Shell_NotifyIcon(NIM_ADD) failed: " + std::to_string(GetLastError()));
        return false;
    }
    iconAdded_ = true;
    // 使用当前 Shell 支持的现代回调约定；旧系统会安全忽略该请求。
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    return true;
}

bool TrayApp::CreateIcon() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayApp::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::Error("RegisterClassExW failed: " + std::to_string(GetLastError()));
        return false;
    }

    hwnd_ = CreateWindowExW(0, kClassName, L"RemoteAssist Tray", 0,
                            0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
        log::Error("CreateWindowExW(tray) failed: " + std::to_string(GetLastError()));
        return false;
    }

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kCallbackMessage;
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid_.szTip, L"RemoteAssist");
    hMenu_ = CreatePopupMenu();
    if (!hMenu_) {
        log::Error("CreatePopupMenu failed: " + std::to_string(GetLastError()));
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    AppendMenuW(hMenu_, MF_STRING, kMenuCommandOpenSetup, L"打开配置窗口");
    AppendMenuW(hMenu_, MF_STRING, kMenuCommandShowPw, L"显示首次访问密码");
    AppendMenuW(hMenu_, MF_STRING, kMenuCommandAbout, L"关于 RemoteAssist");
    AppendMenuW(hMenu_, MF_SEPARATOR, 0, nullptr);
    // 服务会监控 tray 子进程并自动恢复，因此这里不能承诺“退出后不再显示”。
    // 使用准确文案，避免用户误以为该操作会停止服务或永久关闭托盘。
    AppendMenuW(hMenu_, MF_STRING, kMenuCommandExit, L"关闭图标（服务会自动恢复）");
    if (!AddIconToTaskbar()) {
        DestroyMenu(hMenu_);
        hMenu_ = nullptr;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void TrayApp::ShowMenu() {
    if (!hMenu_) {
        return;
    }
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(hMenu_, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

void TrayApp::OpenSetupWindow() {
    const std::wstring exePath = ModulePath();
    if (exePath.empty()) {
        log::Warn("tray cannot resolve executable path: " + std::to_string(GetLastError()));
        return;
    }
    const HINSTANCE result = ShellExecuteW(hwnd_, L"open", exePath.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        log::Warn("tray failed to open setup window: " +
                  std::to_string(reinterpret_cast<INT_PTR>(result)));
    }
}

void TrayApp::ShowPasswordDialog() {
    std::wstring msg;
    const bool hasInitialPassword = !password_.empty();
    if (password_.empty()) {
        msg = L"首次密码已展示过或非首次启动。选择“打开配置窗口”可设置新密码。";
    } else {
        Config cfg;
        const bool configLoaded = LoadConfigReadOnly(cfg);
        msg = L"访问密码(仅本次显示,请妥善保存):\n\n" + password_ +
              (configLoaded
                  ? L"\n\n浏览器打开 http://<本机IP>:" + std::to_wstring(cfg.port) +
                        L"/ 后输入此密码连接。"
                  : L"\n\n未能读取当前端口；请打开配置窗口确认端口后连接。");
    }
    MessageBoxW(hwnd_, msg.c_str(), L"RemoteAssist 访问密码", MB_OK | MB_ICONINFORMATION);
    if (hasInitialPassword) {
        SecureZeroMemory(password_.data(), password_.size() * sizeof(wchar_t));
        password_.clear();
    }
}

void TrayApp::RemoveIcon() {
    if (iconAdded_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        iconAdded_ = false;
    }
    nid_.hWnd = nullptr;
    if (hMenu_) {
        DestroyMenu(hMenu_);
        hMenu_ = nullptr;
    }
}

int TrayApp::Run(const std::wstring& initialPasswordChannel) {
    // Tray 使用 Explorer 的普通用户 token，安全安装目录不授予写权限。日志由
    // LocalSystem 服务通过受控管道转存到同级 logs/tray.log，避免为了诊断放宽 ACL。
    log::InitPipeSink(runtime::kTrayLogPipeName);
    log::Info("tray starting");

    // 保持 mutex 所有权，供 ServiceHost 在没有受管 tray 子进程时识别外部
    // 配置窗口/托盘实例，避免无意义地反复创建重复进程。
    instanceMutex_ = CreateMutexW(nullptr, TRUE, runtime::kTrayMutexName);
    const DWORD mutexError = GetLastError();
    if (!instanceMutex_) {
        log::Error("tray mutex creation failed: " + std::to_string(mutexError));
        return 1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {
        log::Info("tray already running in this session, skip duplicate launch");
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        return 0;
    }

    password_ = ReadInitialPassword(initialPasswordChannel);

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!CreateIcon()) {
        ReleaseMutex(instanceMutex_);
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        return 1;
    }
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
    ReleaseMutex(instanceMutex_);
    CloseHandle(instanceMutex_);
    instanceMutex_ = nullptr;
    log::Info("tray exit");
    return 0;
}

}  // namespace remote_assist
