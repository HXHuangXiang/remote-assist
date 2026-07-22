#include "setup/SetupDialog.h"
#include "common/Config.h"
#include "common/Log.h"

#include <shellapi.h>
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
    IDC_HIDE_TRAY,
    IDC_EXIT,
    IDC_PORT_EDIT,
    IDM_TRAY_SHOW = 2001,
    IDM_TRAY_HIDE,
    IDM_TRAY_EXIT,
};

constexpr UINT kTrayCallback = WM_APP + 1;

static Config g_cfg;
static HWND g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HMENU g_trayMenu = nullptr;
static UINT g_wmTaskbarRestart = 0;
static bool g_trayAdded = false;

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
    std::wstring binPath = L"\"" + ExePath() + L"\" --service";
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
        s = L"\u670d\u52a1\u672a\u5b89\u88c5";
    } else if (ServiceRunning()) {
        s = L"\u670d\u52a1\u8fd0\u884c\u4e2d";
    } else {
        s = L"\u670d\u52a1\u5df2\u5b89\u88c5\u4f46\u672a\u8fd0\u884c";
    }
    SetDlgItemTextW(g_hwnd, IDC_STATUS, s.c_str());

    BOOL installed = ServiceExists();
    BOOL running = ServiceRunning();
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_INSTALL), !installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_UNINSTALL), installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_START), installed && !running);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_STOP), installed && running);
}

// ---- \u6258\u76d8\u56fe\u6807\u7ba1\u7406 ----

static void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayCallback;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"RemoteAssist");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayAdded = true;

    g_trayMenu = CreatePopupMenu();
    AppendMenuW(g_trayMenu, MF_STRING, IDM_TRAY_SHOW, L"\u663e\u793a\u7a97\u53e3");
    AppendMenuW(g_trayMenu, MF_STRING, IDM_TRAY_HIDE, L"\u9690\u85cf\u7a97\u53e3");
    AppendMenuW(g_trayMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_trayMenu, MF_STRING, IDM_TRAY_EXIT, L"\u9000\u51fa");
}

static void RemoveTrayIcon() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
    if (g_trayMenu) {
        DestroyMenu(g_trayMenu);
        g_trayMenu = nullptr;
    }
}

static void ShowTrayMenu(HWND hwnd) {
    if (!g_trayMenu) return;
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(g_trayMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
}

static void HideToTray() {
    ShowWindow(g_hwnd, SW_HIDE);
    wcscpy_s(g_nid.szTip, L"RemoteAssist (hidden)");
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
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

    mk(0, L"STATIC", L"\u5bc6\u7801:", 20, 20, 50, 20, 0);
    mk(IDC_PW_EDIT, L"EDIT", L"", 70, 18, 180, 24, ES_AUTOHSCROLL | WS_BORDER);
    mk(IDC_PW_SAVE, L"BUTTON", L"\u4fdd\u5b58\u5bc6\u7801", 260, 17, 90, 26, BS_PUSHBUTTON);

    mk(0, L"STATIC", L"\u7aef\u53e3:", 20, 55, 50, 20, 0);
    mk(IDC_PORT_EDIT, L"EDIT", L"7980", 70, 53, 60, 24, ES_NUMBER | WS_BORDER);

    mk(0, L"STATIC", L"\u72b6\u6001:", 20, 90, 50, 20, 0);
    mk(IDC_STATUS, L"STATIC", L"...", 70, 90, 280, 20, 0);

    mk(IDC_RUN_AGENT, L"BUTTON", L"\u76f4\u63a5\u8fd0\u884c(\u975e\u670d\u52a1)", 20, 125, 160, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_INSTALL, L"BUTTON", L"\u5b89\u88c5\u5e76\u542f\u52a8\u670d\u52a1", 200, 125, 150, 32, BS_PUSHBUTTON);

    mk(IDC_SVC_START, L"BUTTON", L"\u542f\u52a8\u670d\u52a1", 20, 170, 110, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_STOP, L"BUTTON", L"\u505c\u6b62\u670d\u52a1", 140, 170, 110, 32, BS_PUSHBUTTON);
    mk(IDC_SVC_UNINSTALL, L"BUTTON", L"\u5378\u8f7d\u670d\u52a1", 260, 170, 90, 32, BS_PUSHBUTTON);

    mk(IDC_HIDE_TRAY, L"BUTTON", L"\u9690\u85cf\u5230\u6258\u76d8", 20, 210, 130, 30, BS_PUSHBUTTON);
    mk(IDC_EXIT, L"BUTTON", L"\u9000\u51fa", 160, 210, 80, 30, BS_PUSHBUTTON);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // TaskbarCreated: Explorer \u91cd\u542f\u540e\u91cd\u5efa\u6258\u76d8\u56fe\u6807
    if (msg == g_wmTaskbarRestart && g_wmTaskbarRestart != 0) {
        if (g_trayAdded) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);
        }
        return 0;
    }

    // \u6258\u76d8\u56de\u8c03
    if (msg == kTrayCallback) {
        switch (lp) {
        case WM_LBUTTONDBLCLK:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            wcscpy_s(g_nid.szTip, L"RemoteAssist");
            g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            break;
        case WM_RBUTTONUP:
            ShowTrayMenu(hwnd);
            break;
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls(hwnd);
        AddTrayIcon(hwnd);
        g_cfg = LoadOrCreateConfig();
        if (!g_cfg.initialPassword.empty()) {
            SetDlgItemTextA(hwnd, IDC_PW_EDIT, g_cfg.initialPassword.c_str());
        }
        SetDlgItemTextA(hwnd, IDC_PORT_EDIT, std::to_string(g_cfg.port).c_str());
        UpdateStatus();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TRAY_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        case IDM_TRAY_HIDE:
            HideToTray();
            break;
        case IDM_TRAY_EXIT:
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;
        case IDC_PW_SAVE: {
            char pw[256] = {};
            GetDlgItemTextA(hwnd, IDC_PW_EDIT, pw, sizeof(pw));
            if (pw[0]) {
                SetPassword(g_cfg, pw);
                MessageBoxW(hwnd, L"\u5bc6\u7801\u5df2\u4fdd\u5b58", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, L"\u8bf7\u8f93\u5165\u5bc6\u7801", L"RemoteAssist", MB_OK | MB_ICONWARNING);
            }
            break;
        }
        case IDC_SVC_INSTALL: {
            char pw[256] = {};
            GetDlgItemTextA(hwnd, IDC_PW_EDIT, pw, sizeof(pw));
            if (pw[0]) SetPassword(g_cfg, pw);
            char portStr[16] = {};
            GetDlgItemTextA(hwnd, IDC_PORT_EDIT, portStr, sizeof(portStr));
            if (portStr[0]) { g_cfg.port = atoi(portStr); SaveConfig(g_cfg); }
            if (InstallService()) {
                MessageBoxW(hwnd, L"\u670d\u52a1\u5b89\u88c5\u6210\u529f,\u6b63\u5728\u542f\u52a8...", L"RemoteAssist", MB_OK);
                StartServiceS();
                MessageBoxW(hwnd, L"\u670d\u52a1\u5df2\u542f\u52a8,\u6d4f\u89c8\u5668\u8bbf\u95ee http://\u672c\u673aIP:7980", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                DWORD err = GetLastError();
                wchar_t buf[256];
                swprintf_s(buf, L"\u5b89\u88c5\u5931\u8d25(\u9519\u8bef\u7801 %u),\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c", err);
                MessageBoxW(hwnd, buf, L"RemoteAssist", MB_OK | MB_ICONERROR);
            }
            UpdateStatus();
            break;
        }
        case IDC_SVC_UNINSTALL:
            if (UninstallService()) {
                MessageBoxW(hwnd, L"\u670d\u52a1\u5df2\u5378\u8f7d", L"RemoteAssist", MB_OK);
            } else {
                MessageBoxW(hwnd, L"\u5378\u8f7d\u5931\u8d25,\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c", L"RemoteAssist", MB_OK | MB_ICONERROR);
            }
            UpdateStatus();
            break;
        case IDC_SVC_START:
            if (StartServiceS()) {
                MessageBoxW(hwnd, L"\u670d\u52a1\u5df2\u542f\u52a8", L"RemoteAssist", MB_OK);
            } else {
                DWORD err = GetLastError();
                wchar_t buf[256];
                swprintf_s(buf, L"\u542f\u52a8\u5931\u8d25(\u9519\u8bef\u7801 %u)", err);
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
        case IDC_RUN_AGENT: {
            std::wstring exe = ExePath();
            std::wstring cmd = L"\"" + exe + L"\" --agent";
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, const_cast<LPWSTR>(cmd.c_str()),
                    nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                MessageBoxW(hwnd, L"Agent \u5df2\u542f\u52a8,\u6d4f\u89c8\u5668\u8bbf\u95ee http://\u672c\u673aIP:7980", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, L"\u542f\u52a8 agent \u5931\u8d25", L"RemoteAssist", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case IDC_HIDE_TRAY:
            HideToTray();
            break;
        case IDC_EXIT:
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;
        }
        return 0;

    // \u5173\u95ed\u7a97\u53e3(X \u6309\u94ae):\u9690\u85cf\u5230\u6258\u76d8\u800c\u4e0d\u9000\u51fa
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        wcscpy_s(g_nid.szTip, L"RemoteAssist (hidden)");
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunSetupDialog(HINSTANCE hInst) {
    log::Init(LogDir());
    log::Info("setup dialog starting");

    g_wmTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

    const wchar_t* cls = L"RemoteAssistSetup";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, cls, L"RemoteAssist \u914d\u7f6e",
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
