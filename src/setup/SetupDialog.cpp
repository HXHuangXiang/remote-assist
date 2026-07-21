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
    IDC_RUN_AGENT,
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
    if (!scm) { log::Error("OpenSCManager failed err=" + std::to_string(GetLastError())); return false; }
    std::wstring binPath = L""" + ExePath() + L"" --service";
    SC_HANDLE svc = CreateServiceW(scm, L"remote-assist", L"RemoteAssist",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (svc) { CloseServiceHandle(svc); CloseServiceHandle(scm); return true; }
    log::Error("CreateService failed err=" + std::to_string(GetLastError()));
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
    DWORD err = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok || err == ERROR_SERVICE_ALREADY_RUNNING;
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
    SetDlgItemTextW(g_hwnd, IDC_STATUS, s.c_str());

    BOOL installed = ServiceExists();
    BOOL running = ServiceRunning();
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_INSTALL), !installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_UNINSTALL), installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_START), installed && !running);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_STOP), installed && running);
}

static void CreateControls(HWND hwnd) {
    HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    auto mk = [&](int id, const wchar_t* cls, const wchar_t* text, int x, int y, int w, int h, DWORD style) {
        HWND hw = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
            x, y, w, h, hwnd, (HMENU)(LONG_PTR)id, nullptr, nullptr);
        SendMessageW(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
        return hw;
    };

    mk(0, L"STATIC", L"密码:", 20, 20, 50, 20, 0);
    mk(IDC_PW_EDIT, L"EDIT", L"", 70, 18, 180, 24, ES_AUTOHSCROLL | WS_BORDER);
    mk(IDC_PW_SAVE, L"BUTTON", L"保存密码", 260, 17, 90, 26, BS_PUSHBUTTON);

    mk(0, L"STATIC", L"端口:", 20, 55, 50, 20, 0);
    mk(IDC_PORT_EDIT, L"EDIT", L"7980", 70, 53, 60, 24, ES_NUMBER | WS_BORDER);

    mk(0, L"STATIC", L"状态:", 20, 90, 50, 20, 0);
    mk(IDC_STATUS, L"STATIC", L"...", 70, 90, 280, 20, 0);

    mk(IDC_RUN_AGENT, L"BUTTON", L"直接运行(非服务)", 20, 125, 160, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_INSTALL, L"BUTTON", L"安装并启动服务", 200, 125, 150, 32, BS_PUSHBUTTON);

    mk(IDC_SVC_START, L"BUTTON", L"启动服务", 20, 170, 110, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_STOP, L"BUTTON", L"停止服务", 140, 170, 110, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_UNINSTALL, L"BUTTON", L"卸载服务", 260, 170, 90, 32, BS_PUSHBUTTON);

    mk(IDC_EXIT, L"BUTTON", L"退出", 320, 210, 80, 30, BS_PUSHBUTTON);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls(hwnd);
        g_cfg = LoadOrCreateConfig();
        if (!g_cfg.initialPassword.empty()) {
            SetDlgItemTextA(hwnd, IDC_PW_EDIT, g_cfg.initialPassword.c_str());
        }
        SetDlgItemTextA(hwnd, IDC_PORT_EDIT, std::to_string(g_cfg.port).c_str());
        UpdateStatus();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PW_SAVE: {
            char pw[256] = {};
            GetDlgItemTextA(hwnd, IDC_PW_EDIT, pw, sizeof(pw));
            if (pw[0]) {
                SetPassword(g_cfg, pw);
                MessageBoxW(hwnd, L"密码已保存", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, L"请输入密码", L"RemoteAssist", MB_OK | MB_ICONWARNING);
            }
            break;
        }
        case IDC_SVC_INSTALL: {
            // 先保存配置
            char pw[256] = {};
            GetDlgItemTextA(hwnd, IDC_PW_EDIT, pw, sizeof(pw));
            if (pw[0]) SetPassword(g_cfg, pw);
            char portStr[16] = {};
            GetDlgItemTextA(hwnd, IDC_PORT_EDIT, portStr, sizeof(portStr));
            if (portStr[0]) { g_cfg.port = atoi(portStr); SaveConfig(g_cfg); }
            if (InstallService()) {
                MessageBoxW(hwnd, L"服务安装成功,正在启动...", L"RemoteAssist", MB_OK);
                StartServiceS();
                MessageBoxW(hwnd, L"服务已启动,浏览器访问 http://本机IP:7980", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                DWORD err = GetLastError();
                wchar_t buf[256];
                swprintf_s(buf, L"安装失败(错误码 %u),请以管理员身份运行", err);
                MessageBoxW(hwnd, buf, L"RemoteAssist", MB_OK | MB_ICONERROR);
            }
            UpdateStatus();
            break;
        }
        case IDC_SVC_UNINSTALL:
            if (UninstallService()) {
                MessageBoxW(hwnd, L"服务已卸载", L"RemoteAssist", MB_OK);
            } else {
                MessageBoxW(hwnd, L"卸载失败,请以管理员身份运行", L"RemoteAssist", MB_OK | MB_ICONERROR);
            }
            UpdateStatus();
            break;
        case IDC_SVC_START:
            if (StartServiceS()) {
                MessageBoxW(hwnd, L"服务已启动", L"RemoteAssist", MB_OK);
            } else {
                DWORD err = GetLastError();
                wchar_t buf[256];
                swprintf_s(buf, L"启动失败(错误码 %u)", err);
                MessageBoxW(hwnd, buf, L"RemoteAssist", MB_OK | MB_ICONERROR);
            }
            Sleep(500);
            UpdateStatus();
            break;
        case IDC_SVC_STOP:
            StopServiceS();
            Sleep(500);
            UpdateStatus();
            break;
        case IDC_RUN_AGENT:
            // 直接启动 agent 进程(非服务模式)
            {
                std::wstring exe = ExePath();
                std::wstring cmd = L""" + exe + L"" --agent";
                STARTUPINFOW si = {};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {};
                if (CreateProcessW(nullptr, const_cast<LPWSTR>(cmd.c_str()),
                        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    MessageBoxW(hwnd, L"Agent 已启动,浏览器访问 http://本机IP:7980", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(hwnd, L"启动 agent 失败", L"RemoteAssist", MB_OK | MB_ICONERROR);
                }
            }
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
    // 初始化日志(exe 同目录下的 logs/ 子目录)
    log::Init(LogDir());
    log::Info("setup dialog starting");

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

    if (!hwnd) {
        log::Error("CreateWindowEx failed err=" + std::to_string(GetLastError()));
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    log::Info("setup dialog exit");
    return 0;
}

}  // namespace remote_assist
