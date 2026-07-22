#include "setup/SetupDialog.h"
#include "common/Config.h"
#include "common/Log.h"
#include "common/RuntimeNames.h"

#include <netfw.h>
#include <wrl/client.h>

#include <shellapi.h>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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
static HFONT g_font = nullptr;
constexpr wchar_t kFirewallRuleName[] = L"RemoteAssist 局域网控制（专用网络）";
constexpr wchar_t kFirewallRuleDescription[] =
    L"允许 RemoteAssist 在专用局域网接收浏览器远程协助连接。";
// 与 service 拉起的 TrayApp 共用每会话 mutex，避免配置窗口额外创建托盘图标。
static HANDLE g_trayMutex = nullptr;
static bool g_ownsTray = false;

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

struct ServiceResult {
    bool ok = false;
    DWORD error = ERROR_GEN_FAILURE;
};

struct InstallServiceResult {
    ServiceResult service;
    // 防火墙策略可能被企业策略锁定。服务安装不因该项失败回滚，但 UI 必须明确提示。
    DWORD firewallError = ERROR_SUCCESS;
};

// COM 防火墙接口接收 BSTR。此小型 RAII 包装避免每个错误分支遗漏 SysFreeString。
class ScopedBstr {
public:
    explicit ScopedBstr(std::wstring_view value)
        : value_(SysAllocStringLen(value.data(), static_cast<UINT>(value.size()))) {}
    ~ScopedBstr() { SysFreeString(value_); }

    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    BSTR Get() const { return value_; }
    bool Valid() const { return value_ != nullptr; }

private:
    BSTR value_ = nullptr;
};

DWORD ErrorFromHresult(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return HRESULT_CODE(hr);
    }
    return static_cast<DWORD>(hr);
}

// 为当前 exe 与端口创建一个仅作用于 Windows“专用”网络配置文件的入站规则。
// 规则按名称替换，因此改端口或升级 exe 后不会累积旧规则。
bool ConfigurePrivateFirewallRule(const std::wstring& exePath, int port, DWORD& error) {
    error = ERROR_SUCCESS;
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        error = ErrorFromHresult(comHr);
        return false;
    }
    auto fail = [&](HRESULT hr) {
        if (shouldUninitialize) {
            CoUninitialize();
        }
        error = ErrorFromHresult(hr);
        return false;
    };

    Microsoft::WRL::ComPtr<INetFwPolicy2> policy;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&policy));
    if (FAILED(hr)) {
        return fail(hr);
    }
    Microsoft::WRL::ComPtr<INetFwRules> rules;
    hr = policy->get_Rules(rules.GetAddressOf());
    if (FAILED(hr)) {
        return fail(hr);
    }

    ScopedBstr name(kFirewallRuleName);
    const std::wstring portText = std::to_wstring(port);
    ScopedBstr ports(portText);
    ScopedBstr application(exePath);
    ScopedBstr description(kFirewallRuleDescription);
    if (!name.Valid() || !ports.Valid() || !application.Valid() || !description.Valid()) {
        return fail(E_OUTOFMEMORY);
    }

    // 旧规则不存在时通常会返回“文件未找到”，这不是错误。
    rules->Remove(name.Get());

    Microsoft::WRL::ComPtr<INetFwRule> rule;
    hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&rule));
    if (FAILED(hr) || FAILED(hr = rule->put_Name(name.Get())) ||
        FAILED(hr = rule->put_Description(description.Get())) ||
        FAILED(hr = rule->put_ApplicationName(application.Get())) ||
        FAILED(hr = rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP)) ||
        FAILED(hr = rule->put_LocalPorts(ports.Get())) ||
        FAILED(hr = rule->put_Direction(NET_FW_RULE_DIR_IN)) ||
        FAILED(hr = rule->put_Action(NET_FW_ACTION_ALLOW)) ||
        FAILED(hr = rule->put_Profiles(NET_FW_PROFILE2_PRIVATE)) ||
        FAILED(hr = rule->put_Enabled(VARIANT_TRUE)) ||
        FAILED(hr = rules->Add(rule.Get()))) {
        return fail(hr);
    }

    if (shouldUninitialize) {
        CoUninitialize();
    }
    return true;
}

void RemovePrivateFirewallRule() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        return;
    }
    Microsoft::WRL::ComPtr<INetFwPolicy2> policy;
    Microsoft::WRL::ComPtr<INetFwRules> rules;
    if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&policy))) &&
        SUCCEEDED(policy->get_Rules(rules.GetAddressOf()))) {
        ScopedBstr name(kFirewallRuleName);
        if (name.Valid()) {
            rules->Remove(name.Get());
        }
    }
    if (shouldUninitialize) {
        CoUninitialize();
    }
}

ServiceResult ServiceSuccess() {
    return {true, ERROR_SUCCESS};
}

ServiceResult ServiceFailure(DWORD error) {
    return {false, error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error};
}

// GUI 安装路径与 tools/install.bat 保持一致：异常退出由 SCM 自动拉起，正常
// StopService 不受影响；这样用户双击安装后不会少掉脚本安装才有的恢复能力。
ServiceResult ConfigureServiceRecovery(SC_HANDLE service) {
    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<LPWSTR>(L"RemoteAssist 远控被控端(LocalSystem)");
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description)) {
        return ServiceFailure(GetLastError());
    }

    SC_ACTION actions[] = {
        {SC_ACTION_RESTART, 5000},
        {SC_ACTION_RESTART, 10000},
        {SC_ACTION_NONE, 0},
    };
    SERVICE_FAILURE_ACTIONSW recovery{};
    recovery.dwResetPeriod = 86400;
    recovery.cActions = static_cast<DWORD>(std::size(actions));
    recovery.lpsaActions = actions;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &recovery)) {
        return ServiceFailure(GetLastError());
    }
    return ServiceSuccess();
}

bool QueryServiceState(SC_HANDLE service, SERVICE_STATUS_PROCESS& status, DWORD& error) {
    DWORD bytesNeeded = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
        error = GetLastError();
        return false;
    }
    return true;
}

bool WaitForServiceState(SC_HANDLE service, DWORD targetState, DWORD timeoutMs, DWORD& error) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        SERVICE_STATUS_PROCESS status{};
        if (!QueryServiceState(service, status, error)) {
            return false;
        }
        if (status.dwCurrentState == targetState) {
            return true;
        }
        if (targetState != SERVICE_STOPPED && status.dwCurrentState == SERVICE_STOPPED) {
            error = status.dwWin32ExitCode != ERROR_SUCCESS
                ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
            return false;
        }
        if (GetTickCount64() >= deadline) {
            error = ERROR_TIMEOUT;
            return false;
        }
        Sleep(100);
    }
}

std::wstring ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD chars = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (!chars || !buffer) {
        return L"未知错误";
    }
    std::wstring text(buffer, chars);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

void ShowServiceError(HWND hwnd, const wchar_t* action, DWORD error) {
    const std::wstring message = std::wstring(action) + L"失败（错误 " +
        std::to_wstring(error) + L"）：\n" + ErrorText(error);
    MessageBoxW(hwnd, message.c_str(), L"RemoteAssist", MB_OK | MB_ICONERROR);
}

static bool ServiceExists() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, runtime::kServiceName, SERVICE_QUERY_STATUS);
    bool exists = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}

static bool ServiceRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, runtime::kServiceName, SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }
    SERVICE_STATUS_PROCESS st{};
    DWORD error = ERROR_SUCCESS;
    bool running = QueryServiceState(svc, st, error) && st.dwCurrentState == SERVICE_RUNNING;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

// SCM 报告 RUNNING 只表示服务主进程已进入循环；必须等 agent 成功绑定 HTTP/
// WebSocket 端口后，控制端才真正可以连接。
static bool WaitForAgentReady(DWORD timeoutMs) {
    HANDLE readyEvent = OpenEventW(SYNCHRONIZE, FALSE, runtime::kAgentReadyEventName);
    if (!readyEvent) {
        return false;
    }
    const DWORD result = WaitForSingleObject(readyEvent, timeoutMs);
    CloseHandle(readyEvent);
    return result == WAIT_OBJECT_0;
}

static InstallServiceResult InstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        const DWORD error = GetLastError();
        log::Error("OpenSCManager failed err=" + std::to_string(error));
        return {ServiceFailure(error)};
    }
    std::wstring binPath = L"\"" + ExePath() + L"\" --service";
    SC_HANDLE svc = CreateServiceW(scm, runtime::kServiceName, L"RemoteAssist",
        SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | SERVICE_CHANGE_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (svc) {
        const ServiceResult configured = ConfigureServiceRecovery(svc);
        if (!configured.ok) {
            log::Error("ConfigureServiceRecovery failed err=" +
                       std::to_string(configured.error));
            DeleteService(svc);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return {configured};
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        DWORD firewallError = ERROR_SUCCESS;
        if (!ConfigurePrivateFirewallRule(ExePath(), g_cfg.port, firewallError)) {
            log::Warn("private firewall rule update failed err=" +
                      std::to_string(firewallError));
        }
        return {ServiceSuccess(), firewallError};
    }
    const DWORD error = GetLastError();
    log::Error("CreateService failed err=" + std::to_string(error));
    CloseServiceHandle(scm);
    return {ServiceFailure(error)};
}

static ServiceResult StartServiceS() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return ServiceFailure(GetLastError());
    }
    SC_HANDLE svc = OpenServiceW(scm, runtime::kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        return ServiceFailure(error);
    }

    DWORD error = ERROR_SUCCESS;
    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceState(svc, status, error)) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ServiceFailure(error);
    }
    if (status.dwCurrentState == SERVICE_STOP_PENDING &&
        !WaitForServiceState(svc, SERVICE_STOPPED, 15000, error)) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ServiceFailure(error);
    }
    if (status.dwCurrentState != SERVICE_RUNNING && status.dwCurrentState != SERVICE_START_PENDING &&
        !StartServiceW(svc, 0, nullptr)) {
        error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return ServiceFailure(error);
        }
    }
    const bool ok = WaitForServiceState(svc, SERVICE_RUNNING, 15000, error);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? ServiceSuccess() : ServiceFailure(error);
}

static ServiceResult StopServiceS() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return ServiceFailure(GetLastError());
    }
    SC_HANDLE svc = OpenServiceW(scm, runtime::kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        return ServiceFailure(error);
    }

    DWORD error = ERROR_SUCCESS;
    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceState(svc, status, error)) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ServiceFailure(error);
    }
    if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS ignored{};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &ignored)) {
            error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) {
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                return ServiceFailure(error);
            }
        }
    }
    const bool ok = WaitForServiceState(svc, SERVICE_STOPPED, 15000, error);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? ServiceSuccess() : ServiceFailure(error);
}

static ServiceResult RestartServiceS() {
    ServiceResult result = StopServiceS();
    if (!result.ok) {
        return result;
    }
    return StartServiceS();
}

static ServiceResult UninstallService() {
    ServiceResult stopResult = StopServiceS();
    if (!stopResult.ok && stopResult.error != ERROR_SERVICE_DOES_NOT_EXIST) {
        return stopResult;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return ServiceFailure(GetLastError());
    }
    SC_HANDLE svc = OpenServiceW(scm, runtime::kServiceName, DELETE);
    if (!svc) {
        const DWORD error = GetLastError();
        CloseServiceHandle(scm);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            // 遗留规则也必须清理，例如服务曾被手工删除的情况。
            RemovePrivateFirewallRule();
            return ServiceSuccess();
        }
        return ServiceFailure(error);
    }
    const bool ok = DeleteService(svc) != FALSE;
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (ok) {
        RemovePrivateFirewallRule();
    }
    return ok ? ServiceSuccess() : ServiceFailure(error);
}

bool ReadPort(HWND hwnd, int& port) {
    char text[16] = {};
    GetDlgItemTextA(hwnd, IDC_PORT_EDIT, text, sizeof(text));
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (text[0] == '\0' || !end || *end != '\0' || value < 1 || value > 65535) {
        MessageBoxW(hwnd, L"端口必须是 1 到 65535 的整数。", L"RemoteAssist", MB_OK | MB_ICONWARNING);
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

bool SaveSettingsFromControls(HWND hwnd, bool& changed, bool& portChanged) {
    int port = 0;
    if (!ReadPort(hwnd, port)) {
        return false;
    }

    char password[256] = {};
    GetDlgItemTextA(hwnd, IDC_PW_EDIT, password, sizeof(password));
    const bool passwordChanged = password[0] != '\0';
    portChanged = g_cfg.port != port;
    changed = passwordChanged || portChanged;
    if (!changed) {
        return true;
    }

    const Config original = g_cfg;
    g_cfg.port = port;
    const bool saved = passwordChanged ? SetPassword(g_cfg, password) : SaveConfig(g_cfg);
    if (!saved) {
        g_cfg = original;
        MessageBoxW(hwnd, L"配置保存失败，请检查 exe 目录写权限。", L"RemoteAssist", MB_OK | MB_ICONERROR);
        return false;
    }
    SetDlgItemTextA(hwnd, IDC_PW_EDIT, "");
    return true;
}

static void UpdateStatus() {
    std::wstring s;
    if (!ServiceExists()) {
        s = L"\u670d\u52a1\u672a\u5b89\u88c5";
    } else if (ServiceRunning()) {
        s = WaitForAgentReady(0) ?
            L"\u670d\u52a1\u8fd0\u884c\u4e2d\uff08Agent \u5df2\u5c31\u7eea\uff09" :
            L"\u670d\u52a1\u8fd0\u884c\u4e2d\uff08\u7b49\u5f85 Agent\uff09";
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
    // 服务已经接管当前桌面时不再允许额外启动独立 Agent；停止服务后仍可保留
    // 该调试/临时使用入口，服务再次启动会通过停止事件完成接管。
    EnableWindow(GetDlgItem(g_hwnd, IDC_RUN_AGENT), !running);
}

// ---- \u6258\u76d8\u56fe\u6807\u7ba1\u7406 ----

static void AddTrayIcon(HWND hwnd) {
    if (!g_ownsTray) {
        return;
    }
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

// 服务接管托盘前，配置窗口必须释放当前会话的 mutex；否则 service 启动的
// TrayApp 会因重复实例立刻退出，ServiceHost 又会把它误当作崩溃反复拉起。
static void RelinquishTrayOwnership() {
    RemoveTrayIcon();
    if (g_trayMutex) {
        if (g_ownsTray) {
            ReleaseMutex(g_trayMutex);
        }
        CloseHandle(g_trayMutex);
        g_trayMutex = nullptr;
    }
    g_ownsTray = false;
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
    if (!g_ownsTray || !g_trayAdded) {
        return;
    }
    wcscpy_s(g_nid.szTip, L"RemoteAssist (hidden)");
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void CreateControls(HWND hwnd) {
    g_font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    auto mk = [&](int id, const wchar_t* cls, const wchar_t* text, int x, int y, int w, int h, DWORD style) {
        HWND hw = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
            x, y, w, h, hwnd, (HMENU)(LONG_PTR)id, nullptr, nullptr);
        if (g_font) {
            SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
        return hw;
    };

    mk(0, L"STATIC", L"\u5bc6\u7801(\u7559\u7a7a\u4e0d\u53d8):", 20, 20, 120, 20, 0);
    mk(IDC_PW_EDIT, L"EDIT", L"", 140, 18, 110, 24,
       ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER);
    mk(IDC_PW_SAVE, L"BUTTON", L"\u4fdd\u5b58\u914d\u7f6e", 260, 17, 90, 26, BS_PUSHBUTTON);

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
            bool changed = false;
            bool portChanged = false;
            if (!SaveSettingsFromControls(hwnd, changed, portChanged)) {
                break;
            }
            if (!changed) {
                MessageBoxW(hwnd, L"配置没有变化。", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
                break;
            }
            if (ServiceRunning()) {
                const ServiceResult restart = RestartServiceS();
                if (!restart.ok) {
                    MessageBoxW(hwnd,
                                L"配置已保存，但服务重启失败；新配置会在下次服务启动时生效。",
                                L"RemoteAssist", MB_OK | MB_ICONWARNING);
                    ShowServiceError(hwnd, L"重启服务", restart.error);
                } else {
                    MessageBoxW(hwnd, L"配置已保存，服务已重启并应用新密码/端口。",
                                L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
                }
            } else {
                MessageBoxW(hwnd, L"配置已保存，将在下次启动 Agent 或服务时生效。",
                            L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            }
            if (portChanged && ServiceExists()) {
                DWORD firewallError = ERROR_SUCCESS;
                if (!ConfigurePrivateFirewallRule(ExePath(), g_cfg.port, firewallError)) {
                    ShowServiceError(hwnd, L"更新专用网络防火墙规则", firewallError);
                }
            }
            UpdateStatus();
            break;
        }
        case IDC_SVC_INSTALL: {
            bool changed = false;
            bool portChanged = false;
            if (!SaveSettingsFromControls(hwnd, changed, portChanged)) {
                break;
            }
            const InstallServiceResult install = InstallService();
            if (!install.service.ok) {
                ShowServiceError(hwnd, L"安装服务", install.service.error);
                UpdateStatus();
                break;
            }
            RelinquishTrayOwnership();
            const ServiceResult start = StartServiceS();
            if (!start.ok) {
                ShowServiceError(hwnd, L"启动服务", start.error);
            } else {
                const bool agentReady = WaitForAgentReady(5000);
                std::wstring message;
                if (agentReady) {
                    message = L"服务与 Agent 已启动。浏览器访问 http://<本机IP>:" +
                        std::to_wstring(g_cfg.port) + L"/";
                } else {
                    message = L"服务已启动。Agent 尚未就绪（等待登录桌面或查看 logs\\service.log）。";
                }
                if (install.firewallError != ERROR_SUCCESS) {
                    message += L"\n\n未能自动添加专用网络防火墙规则；局域网访问可能被阻止。";
                }
                MessageBoxW(hwnd, message.c_str(), L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            }
            UpdateStatus();
            break;
        }
        case IDC_SVC_UNINSTALL: {
            const ServiceResult uninstall = UninstallService();
            if (uninstall.ok) {
                MessageBoxW(hwnd, L"服务已卸载。", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                ShowServiceError(hwnd, L"卸载服务", uninstall.error);
            }
            UpdateStatus();
            break;
        }
        case IDC_SVC_START: {
            RelinquishTrayOwnership();
            const ServiceResult start = StartServiceS();
            if (start.ok) {
                const bool agentReady = WaitForAgentReady(5000);
                MessageBoxW(hwnd,
                    agentReady ? L"服务与 Agent 已启动。" :
                                 L"服务已启动。Agent 尚未就绪（等待登录桌面或查看 logs\\service.log）。",
                    L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                ShowServiceError(hwnd, L"启动服务", start.error);
            }
            UpdateStatus();
            break;
        }
        case IDC_SVC_STOP: {
            const ServiceResult stop = StopServiceS();
            if (stop.ok) {
                MessageBoxW(hwnd, L"服务已停止。", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                ShowServiceError(hwnd, L"停止服务", stop.error);
            }
            UpdateStatus();
            break;
        }
        case IDC_RUN_AGENT: {
            std::wstring exe = ExePath();
            std::wstring cmd = L"\"" + exe + L"\" --agent";
            std::vector<wchar_t> commandLine(cmd.begin(), cmd.end());
            commandLine.push_back(L'\0');
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, commandLine.data(),
                    nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                const std::wstring message = L"Agent 已启动。浏览器访问 http://<本机IP>:" +
                    std::to_wstring(g_cfg.port) + L"/";
                MessageBoxW(hwnd, message.c_str(), L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(hwnd, L"\u542f\u52a8 agent \u5931\u8d25", L"RemoteAssist", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case IDC_HIDE_TRAY:
            if (g_ownsTray || g_trayMutex) {
                HideToTray();
            }
            break;
        case IDC_EXIT:
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;
        }
        return 0;

    // \u5173\u95ed\u7a97\u53e3(X \u6309\u94ae):\u9690\u85cf\u5230\u6258\u76d8\u800c\u4e0d\u9000\u51fa
    case WM_CLOSE:
        if (g_ownsTray) {
            HideToTray();
        } else {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_font) {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunSetupDialog(HINSTANCE hInst) {
    log::Init(LogDir(), L"setup.log");
    log::Info("setup dialog starting");

    g_trayMutex = CreateMutexW(nullptr, TRUE, runtime::kTrayMutexName);
    if (!g_trayMutex) {
        log::Warn("setup tray mutex creation failed: " + std::to_string(GetLastError()));
    } else {
        g_ownsTray = GetLastError() != ERROR_ALREADY_EXISTS;
        if (!g_ownsTray) {
            log::Info("existing session tray detected, setup will not add a second icon");
        }
    }

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
    RemoveTrayIcon();
    if (g_trayMutex) {
        if (g_ownsTray) {
            ReleaseMutex(g_trayMutex);
        }
        CloseHandle(g_trayMutex);
        g_trayMutex = nullptr;
    }
    g_ownsTray = false;
    log::Info("setup dialog exit");
    return 0;
}

}  // namespace remote_assist
