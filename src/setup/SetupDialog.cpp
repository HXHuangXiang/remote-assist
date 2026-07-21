#include "setup/SetupDialog.h"
#include "common/Config.h"
#include "common/Log.h"

#include <string>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace remote_assist {

enum {
    IDC_PW_EDIT = 1001,
    IDC_PW_SAVE,
    IDC_STATUS,
    IDC_SVC_INSTALL,
    IDC_SVC_UNINSTALL,
    IDC_SVC_START,
    IDC_SVC_STOP,
    IDC_EXIT,
    IDC_PORT_EDIT
};

static Config g_cfg;
static HWND g_hwnd = nullptr;

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static bool ServiceExists() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"remote-assist", SERVICE_QUERY_STATUS);
    bool exists = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

static bool ServiceRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"remote-assist", SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }
    SERVICE_STATUS st{};
    QueryServiceStatus(svc, &st);
    bool running = (st.dwCurrentState == SERVICE_RUNNING);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

static bool InstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;
    std::wstring binPath = L""" + ExePath() + L"" --service";
    SC_HANDLE svc = CreateServiceW(scm, L"remote-assist", L"RemoteAssist",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (svc) { CloseServiceHandle(svc); CloseServiceHandle(scm); return true; }
    CloseServiceHandle(scm);
    return false;
}

static bool UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"remote-assist", SERVICE_STOP | DELETE);
    if (!svc) { CloseServiceHandle(scm); return false; }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    bool ok = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

static bool StartServiceS() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"remote-assist", SERVICE_START);
    if (!svc) { CloseServiceHandle(scm); return false; }
    bool ok = StartServiceW(svc, 0, nullptr);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
}

static bool StopServiceS() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"remote-assist", SERVICE_STOP);
    if (!svc) { CloseServiceHandle(scm); return false; }
    SERVICE_STATUS st{};
    bool ok = ControlService(svc, SERVICE_CONTROL_STOP, &st);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

static void UpdateStatus() {
    std::wstring s;
    if (!ServiceExists()) {
        s = L"服务未安装";
    } else if (ServiceRunning()) {
        s = L"服务运行中";
    } else {
        s = L"服务已安装但未运行";
    }
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_STATUS), s.c_str());

    BOOL installed = ServiceExists();
    BOOL running = ServiceRunning();
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_INSTALL), !installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_UNINSTALL), installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_START), installed && !running);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_STOP), installed && running);
}

static void CreateControls(HWND hwnd) {
    // 字体
    HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    auto mk = [&](int id, const wchar_t* cls, const wchar_t* text, int x, int y, int w, int h, DWORD style) {
        HWND hw = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
            x, y, w, h, hwnd, (HMENU)(LONG_PTR)id, nullptr, nullptr);
        SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
        return hw;
    };

    mk(0, L"STATIC", L"密码:", 20, 20, 60, 20, 0);
    mk(IDC_PW_EDIT, L"EDIT", L"", 80, 18, 200, 24, ES_AUTOHSCROLL | WS_BORDER);
    mk(IDC_PW_SAVE, L"BUTTON", L"保存密码", 300, 17, 90, 26, BS_PUSHBUTTON);

    mk(0, L"STATIC", L"端口:", 20, 60, 60, 20, 0);
    mk(IDC_PORT_EDIT, L"EDIT", L"7980", 80, 58, 80, 24, ES_NUMBER | WS_BORDER);

    mk(0, L"STATIC", L"状态:", 20, 100, 60, 20, 0);
    mk(IDC_STATUS, L"STATIC", L"...", 80, 100, 310, 20, 0);

    mk(IDC_SVC_INSTALL, L"BUTTON", L"安装并启动服务", 20, 140, 160, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_UNINSTALL, L"BUTTON", L"卸载服务", 200, 140, 100, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_START, L"BUTTON", L"启动服务", 20, 185, 120, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_STOP, L"BUTTON", L"停止服务", 150, 185, 120, 32, BS_PUSHBUTTON);
    mk(IDC_EXIT, L"BUTTON", L"退出", 310, 185, 80, 32, BS_PUSHBUTTON);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls(hwnd);
        // 加载配置,填入密码(如果有初始密码)和端口
        g_cfg = LoadOrCreateConfig();
        if (!g_cfg.initialPassword.empty()) {
            SetWindowTextA(GetDlgItem(hwnd, IDC_PW_EDIT), g_cfg.initialPassword.c_str());
        } else {
            SetWindowTextA(GetDlgItem(hwnd, IDC_PW_EDIT), "");
        }
        SetWindowTextA(GetDlgItem(hwnd, IDC_PORT_EDIT), std::to_string(g_cfg.port).c_str());
        UpdateStatus();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PW_SAVE: {
            char pw[256] = {};
            GetWindowTextA(GetDlgItem(hwnd, IDC_PW_EDIT), pw, sizeof(pw));
            if (pw[0]) {
                SetPassword(g_cfg, pw);
                MessageBoxW(hwnd, L"密码已保存", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            }
            break;
        }
        case IDC_SVC_INSTALL:
            // 保存密码和端口到配置
            {
                char pw[256] = {};
                GetWindowTextA(GetDlgItem(hwnd, IDC_PW_EDIT), pw, sizeof(pw));
                if (pw[0]) SetPassword(g_cfg, pw);
                char portStr[16] = {};
                GetWindowTextA(GetDlgItem(hwnd, IDC_PORT_EDIT), portStr, sizeof(portStr));
                if (portStr[0]) { g_cfg.port = atoi(portStr); SaveConfig(g_cfg); }
            }
            if (InstallService()) {
                StartServiceS();
                Sleep(1000);
                if (ServiceRunning()) {
                    wchar_t msg[256];
                    swprintf_s(msg, 256, L"服务安装并启动成功!浏览器打开 http://localhost:%d 访问。", g_cfg.port);
                    MessageBoxW(hwnd, msg, L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
                } else {
                    wchar_t msg[256];
                    swprintf_s(msg, 256, L"服务已安装但启动失败(错误码 %d)。请查看 exe 目录下 logs/ 的日志。", (int)GetLastError());
                    MessageBoxW(hwnd, msg, L"RemoteAssist", MB_OK | MB_ICONWARNING);
                }
                UpdateStatus();
            } else {
                wchar_t msg[256];
                swprintf_s(msg, 256, L"安装服务失败(错误码 %d)。请右键 exe 以管理员身份运行。", (int)GetLastError());
                MessageBoxW(hwnd, msg, L"RemoteAssist", MB_OK | MB_ICONWARNING);
            }
            break;
        case IDC_SVC_UNINSTALL:
            UninstallService();
            UpdateStatus();
            break;
        case IDC_SVC_START:
            StartServiceS();
            Sleep(500);
            UpdateStatus();
            break;
        case IDC_SVC_STOP:
            StopServiceS();
            Sleep(500);
            UpdateStatus();
            break;
        case IDC_EXIT:
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunSetupDialog(HINSTANCE hInst) {
    const wchar_t* cls = L"RemoteAssistSetup";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, cls, L"RemoteAssist 配置",
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 280, nullptr, nullptr, hInst, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace remote_assist
