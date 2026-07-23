#include "setup/SetupDialog.h"
#include "common/Config.h"
#include "common/Log.h"
#include "common/Path.h"
#include "common/RuntimeNames.h"

#include <netfw.h>
#include <wrl/client.h>

#include <shellapi.h>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
    IDC_QUALITY_COMBO,
    IDM_TRAY_SHOW = 2001,
    IDM_TRAY_HIDE,
    IDM_TRAY_EXIT,
};

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kAsyncOperationCompleted = WM_APP + 2;
constexpr UINT_PTR kStatusRefreshTimerId = 1;
constexpr UINT kStatusRefreshIntervalMs = 1000;

static Config g_cfg;
static HWND g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HMENU g_trayMenu = nullptr;
static UINT g_wmTaskbarRestart = 0;
static bool g_trayAdded = false;
static HFONT g_font = nullptr;
constexpr UINT kBaseDpi = 96;
constexpr wchar_t kFirewallRuleName[] = L"RemoteAssist 局域网控制（专用网络）";
constexpr wchar_t kFirewallRuleDescription[] =
    L"允许 RemoteAssist 在专用局域网接收浏览器远程协助连接。";
// 与 service 拉起的 TrayApp 共用每会话 mutex，避免配置窗口额外创建托盘图标。
static HANDLE g_trayMutex = nullptr;
static bool g_ownsTray = false;

static std::wstring ExePath() {
    return ModulePath();
}

// 配置窗口使用 Per-Monitor V2。所有手工指定的坐标都要随当前 DPI 放大，避免在
// 高分屏上仍以 96 DPI 的物理像素绘制，导致文字和按钮过小。
int ScaleByDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi == 0 ? kBaseDpi : dpi), kBaseDpi);
}

UINT WindowDpi(HWND hwnd) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return dpi == 0 ? kBaseDpi : dpi;
}

// 浏览器通过 WebSocket JSON 发送 UTF-8 token；配置窗口也必须把用户输入转换为同一
// 字节序列后才进入 PBKDF2，不能继续依赖当前系统 ACP 的 GetDlgItemTextA。
bool ReadPasswordUtf8(HWND hwnd, std::string& password) {
    password.clear();
    const HWND edit = GetDlgItem(hwnd, IDC_PW_EDIT);
    if (!edit) {
        return false;
    }
    const int wideLength = GetWindowTextLengthW(edit);
    if (wideLength <= 0) {
        return true;
    }
    std::vector<wchar_t> wide(static_cast<size_t>(wideLength) + 1, L'\0');
    const int copied = GetWindowTextW(edit, wide.data(), static_cast<int>(wide.size()));
    if (copied != wideLength) {
        return false;
    }
    const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                wide.data(), copied, nullptr, 0,
                                                nullptr, nullptr);
    // JSON 会转义引号、反斜杠等字符；3072 字节即使全部需要双字节转义，也能安全
    // 落在 8 KiB WebSocket 首帧上限以内。
    if (utf8Length <= 0 || utf8Length > 3072) {
        return false;
    }
    password.resize(static_cast<size_t>(utf8Length));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), copied,
                               password.data(), utf8Length, nullptr, nullptr) == utf8Length;
}

// 首次随机密码保存在 Config 的 UTF-8 临时字段中；图形界面展示时必须严格按 UTF-8
// 转回宽字符，不能依赖当前系统 ANSI 代码页。
std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length) != length) {
        return {};
    }
    return result;
}

void SecureClearString(std::string& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

struct ServiceResult {
    bool ok = false;
    DWORD error = ERROR_GEN_FAILURE;
};

struct InstallServiceResult {
    ServiceResult service;
    // 防火墙策略可能被企业策略锁定。服务安装不因该项失败回滚，但 UI 必须明确提示。
    DWORD firewallError = ERROR_SUCCESS;
    // 当前目录中的新版本替换已有服务映像时，必须重启正在运行的旧服务，新的
    // binPath 才会实际生效。
    bool updatedExistingService = false;
};

// 服务控制、等待 SCM 状态与防火墙 COM 调用都可能阻塞数秒。它们不能运行在窗口
// 线程中，否则用户会看到按钮按下后整个配置窗口无响应。
enum class AsyncOperation {
    kInstall,
    kStart,
    kStop,
    kUninstall,
    kRestart,
    kUpdateFirewall,
};

struct AsyncOperationResult {
    AsyncOperation operation = AsyncOperation::kStart;
    InstallServiceResult install;
    ServiceResult service;
    DWORD firewallError = ERROR_SUCCESS;
    bool agentReady = false;
};

static bool g_operationInProgress = false;
static bool g_startAfterInstall = false;
static DWORD g_installFirewallError = ERROR_SUCCESS;

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

// 图形安装创建的服务在异常退出时由 SCM 自动拉起，正常 StopService 不受影响。
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

// 状态栏需要区分“尚未启动”和“服务已启动后立即以错误码退出”。后者通常是安装目录
// ACL、配置文件或受管 IPC 初始化失败，若只显示“未运行”会掩盖最有价值的诊断信息。
static bool QueryInstalledServiceStatus(SERVICE_STATUS_PROCESS& status, DWORD& error) {
    error = ERROR_SUCCESS;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = GetLastError();
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, runtime::kServiceName, SERVICE_QUERY_STATUS);
    if (!svc) {
        error = GetLastError();
        CloseServiceHandle(scm);
        return false;
    }
    const bool ok = QueryServiceState(svc, status, error);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
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

// 空闲 Agent 不会持续抓屏，因此该事件未置位通常只表示尚未有控制端触发首帧，
// 而不是服务故障。它用于把“端口可连接”与“画面管线已验证”明确区分。
static bool WaitForAgentFrameReady(DWORD timeoutMs) {
    HANDLE frameReadyEvent = OpenEventW(SYNCHRONIZE, FALSE,
                                        runtime::kAgentFrameReadyEventName);
    if (!frameReadyEvent) {
        return false;
    }
    const DWORD result = WaitForSingleObject(frameReadyEvent, timeoutMs);
    CloseHandle(frameReadyEvent);
    return result == WAIT_OBJECT_0;
}

static InstallServiceResult InstallService() {
    const std::wstring exePath = ExePath();
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr,
                                   SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        const DWORD error = GetLastError();
        log::Error("OpenSCManager failed err=" + std::to_string(error));
        return {ServiceFailure(error)};
    }
    std::wstring binPath = L"\"" + exePath + L"\" --service";
    SC_HANDLE svc = CreateServiceW(scm, runtime::kServiceName, L"RemoteAssist",
        SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | SERVICE_CHANGE_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    bool updatedExistingService = false;
    if (!svc) {
        const DWORD createError = GetLastError();
        if (createError != ERROR_SERVICE_EXISTS) {
            log::Error("CreateService failed err=" + std::to_string(createError));
            CloseServiceHandle(scm);
            return {ServiceFailure(createError)};
        }

        // 双击新版本后可直接“安装/更新”，无需先手工卸载服务；更新时固定服务的
        // 启动类型、账户和映像路径，避免旧配置遗留到新版。
        svc = OpenServiceW(scm, runtime::kServiceName,
                           SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP |
                               SERVICE_CHANGE_CONFIG | DELETE);
        if (!svc) {
            const DWORD error = GetLastError();
            log::Error("OpenService for update failed err=" + std::to_string(error));
            CloseServiceHandle(scm);
            return {ServiceFailure(error)};
        }
        if (!ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                  SERVICE_ERROR_NORMAL, binPath.c_str(), nullptr, nullptr,
                                  nullptr, L"LocalSystem", nullptr, L"RemoteAssist")) {
            const DWORD error = GetLastError();
            log::Error("ChangeServiceConfig(update) failed err=" + std::to_string(error));
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return {ServiceFailure(error)};
        }
        updatedExistingService = true;
    }

    const ServiceResult configured = ConfigureServiceRecovery(svc);
    if (!configured.ok) {
        log::Error("ConfigureServiceRecovery failed err=" +
                   std::to_string(configured.error));
        // 只有本轮创建的新服务才回滚；更新已有服务时不能因附加元数据失败误删用户
        // 已安装的服务配置。
        if (!updatedExistingService) {
            DeleteService(svc);
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return {configured};
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    DWORD firewallError = ERROR_SUCCESS;
    if (!ConfigurePrivateFirewallRule(exePath, g_cfg.port, firewallError)) {
        log::Warn("private firewall rule update failed err=" +
                  std::to_string(firewallError));
    }
    return {ServiceSuccess(), firewallError, updatedExistingService};
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

// 所有慢服务操作都在独立线程完成。结果对象的所有权在成功 PostMessage 后交给
// WndProc；窗口已销毁时 PostMessage 失败，由工作线程释放，避免悬挂操作泄漏内存。
static void StartAsyncOperation(HWND hwnd, AsyncOperation operation, bool updateFirewall = false) {
    if (!hwnd || g_operationInProgress) {
        return;
    }
    g_operationInProgress = true;

    const wchar_t* status = L"正在执行服务操作...";
    switch (operation) {
    case AsyncOperation::kInstall:
        status = L"正在安装/更新服务...";
        break;
    case AsyncOperation::kStart:
        status = L"正在启动服务并等待 Agent...";
        break;
    case AsyncOperation::kStop:
        status = L"正在停止服务...";
        break;
    case AsyncOperation::kUninstall:
        status = L"正在卸载服务...";
        break;
    case AsyncOperation::kRestart:
        status = L"正在重启服务并应用配置...";
        break;
    case AsyncOperation::kUpdateFirewall:
        status = L"正在更新专用网络防火墙规则...";
        break;
    }
    SetDlgItemTextW(hwnd, IDC_STATUS, status);
    constexpr int kBusyControls[] = {
        IDC_PW_EDIT, IDC_PW_SAVE, IDC_PORT_EDIT, IDC_QUALITY_COMBO, IDC_SVC_INSTALL, IDC_SVC_UNINSTALL,
        IDC_SVC_START, IDC_SVC_STOP, IDC_RUN_AGENT, IDC_HIDE_TRAY, IDC_EXIT,
    };
    for (const int id : kBusyControls) {
        EnableWindow(GetDlgItem(hwnd, id), FALSE);
    }

    std::thread([hwnd, operation, updateFirewall] {
        auto result = std::make_unique<AsyncOperationResult>();
        result->operation = operation;
        switch (operation) {
        case AsyncOperation::kInstall:
            result->install = InstallService();
            break;
        case AsyncOperation::kStart:
            result->service = StartServiceS();
            if (result->service.ok) {
                result->agentReady = WaitForAgentReady(5000);
            }
            break;
        case AsyncOperation::kStop:
            result->service = StopServiceS();
            break;
        case AsyncOperation::kUninstall:
            result->service = UninstallService();
            break;
        case AsyncOperation::kRestart:
            result->service = RestartServiceS();
            if (result->service.ok) {
                result->agentReady = WaitForAgentReady(5000);
            }
            if (updateFirewall && result->service.ok &&
                !ConfigurePrivateFirewallRule(ExePath(), g_cfg.port, result->firewallError)) {
                log::Warn("private firewall rule update failed err=" +
                          std::to_string(result->firewallError));
            }
            break;
        case AsyncOperation::kUpdateFirewall:
            if (!ConfigurePrivateFirewallRule(ExePath(), g_cfg.port, result->firewallError)) {
                log::Warn("private firewall rule update failed err=" +
                          std::to_string(result->firewallError));
            }
            break;
        }
        if (!PostMessageW(hwnd, kAsyncOperationCompleted, 0,
                          reinterpret_cast<LPARAM>(result.get()))) {
            log::Warn("setup window closed before service operation completed");
            return;
        }
        result.release();
    }).detach();
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

// ComboBox 的顺序与界面文案绑定，而实际持久化值使用 QualityCap 的底层整数；集中
// 转换避免将控件下标直接写进 config.json，未来调整展示顺序也不会破坏已有配置。
int QualityCapFromComboSelection(LRESULT selection) {
    switch (selection) {
    case 0:
        return static_cast<int>(QualityCap::kAutomatic);
    case 1:
        return static_cast<int>(QualityCap::k1080p);
    case 2:
        return static_cast<int>(QualityCap::k720p);
    case 3:
        return static_cast<int>(QualityCap::k540p);
    default:
        return -1;
    }
}

int ComboSelectionForQualityCap(int qualityCap) {
    switch (static_cast<QualityCap>(qualityCap)) {
    case QualityCap::kAutomatic:
        return 0;
    case QualityCap::k1080p:
        return 1;
    case QualityCap::k720p:
        return 2;
    case QualityCap::k540p:
        return 3;
    }
    return 0;
}

bool ReadQualityCap(HWND hwnd, int& qualityCap) {
    const HWND combo = GetDlgItem(hwnd, IDC_QUALITY_COMBO);
    if (!combo) {
        return false;
    }
    qualityCap = QualityCapFromComboSelection(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (!IsQualityCapValid(qualityCap)) {
        MessageBoxW(hwnd, L"请选择有效的画质上限。", L"RemoteAssist",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

bool SaveSettingsFromControls(HWND hwnd, bool& changed, bool& portChanged) {
    int port = 0;
    if (!ReadPort(hwnd, port)) {
        return false;
    }
    int qualityCap = 0;
    if (!ReadQualityCap(hwnd, qualityCap)) {
        return false;
    }

    std::string password;
    if (!ReadPasswordUtf8(hwnd, password)) {
        MessageBoxW(hwnd,
                    L"密码必须是有效文本，且 UTF-8 编码后不能超过 3072 字节。",
                    L"RemoteAssist", MB_OK | MB_ICONWARNING);
        return false;
    }
    const bool passwordChanged = !password.empty();
    portChanged = g_cfg.port != port;
    const bool qualityCapChanged = g_cfg.qualityCap != qualityCap;
    changed = passwordChanged || portChanged || qualityCapChanged;
    if (!changed) {
        return true;
    }

    const Config original = g_cfg;
    g_cfg.port = port;
    g_cfg.qualityCap = qualityCap;
    const bool saved = passwordChanged ? SetPassword(g_cfg, password) : SaveConfig(g_cfg);
    // PBKDF2 已在 SetPassword 内完成，配置文件只保留哈希；编辑框清空前也要擦除
    // 这个短生命周期 UTF-8 副本，避免明文在堆中额外停留到函数返回之后。
    SecureClearString(password);
    if (!saved) {
        g_cfg = original;
        MessageBoxW(hwnd, L"配置保存失败，请检查 exe 目录写权限。", L"RemoteAssist", MB_OK | MB_ICONERROR);
        return false;
    }
    SetDlgItemTextW(hwnd, IDC_PW_EDIT, L"");
    return true;
}

static void UpdateStatus() {
    const BOOL configValid = !g_cfg.passwordHash.empty() && !g_cfg.salt.empty();
    SERVICE_STATUS_PROCESS serviceStatus{};
    DWORD serviceStatusError = ERROR_SUCCESS;
    const bool serviceStatusKnown =
        QueryInstalledServiceStatus(serviceStatus, serviceStatusError);
    std::wstring s;
    if (!configValid) {
        s = L"配置文件无效：设置新密码后保存即可修复";
    } else if (!serviceStatusKnown && serviceStatusError == ERROR_SERVICE_DOES_NOT_EXIST) {
        s = L"\u670d\u52a1\u672a\u5b89\u88c5";
    } else if (!serviceStatusKnown) {
        s = L"无法读取服务状态（错误 " + std::to_wstring(serviceStatusError) + L"）：" +
            ErrorText(serviceStatusError);
    } else if (serviceStatus.dwCurrentState == SERVICE_RUNNING) {
        if (!WaitForAgentReady(0)) {
            s = L"服务运行中（等待 Agent 监听）";
        } else if (WaitForAgentFrameReady(0)) {
            s = L"服务运行中（画面管线已就绪）";
        } else {
            s = L"服务运行中（Agent 已监听，等待首帧验证）";
        }
    } else if (serviceStatus.dwCurrentState == SERVICE_STOPPED &&
               serviceStatus.dwWin32ExitCode != ERROR_SUCCESS) {
        s = L"服务启动失败（错误 " +
            std::to_wstring(serviceStatus.dwWin32ExitCode) + L"）：" +
            ErrorText(serviceStatus.dwWin32ExitCode);
    } else {
        s = L"\u670d\u52a1\u5df2\u5b89\u88c5\u4f46\u672a\u8fd0\u884c";
    }
    SetDlgItemTextW(g_hwnd, IDC_STATUS, s.c_str());

    BOOL installed = ServiceExists();
    BOOL running = ServiceRunning();
    // 无有效访问密码时即使服务进程可启动，Agent 也会主动退出。禁用安装、启动和
    // 独立 Agent 入口，提示用户先修复配置；停止、卸载仍保留用于恢复已有安装。
    // 已有服务也必须允许“安装/更新”：InstallService 会更新 binPath、启动类型和
    // 恢复策略。若仍沿用 !installed，用户替换 exe 后根本没有入口让 SCM 指向新版本。
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_INSTALL), configValid);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_UNINSTALL), installed);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_START), installed && !running && configValid);
    EnableWindow(GetDlgItem(g_hwnd, IDC_SVC_STOP), installed && running);
    // 服务已经接管当前桌面时不再允许额外启动独立 Agent；停止服务后仍可保留
    // 该调试/临时使用入口，服务再次启动会通过停止事件完成接管。
    EnableWindow(GetDlgItem(g_hwnd, IDC_RUN_AGENT), !running && configValid);
}

// ---- \u6258\u76d8\u56fe\u6807\u7ba1\u7406 ----

static void AddTrayIcon(HWND hwnd) {
    if (!g_ownsTray || g_trayAdded) {
        return;
    }
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayCallback;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"RemoteAssist");
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        log::Warn("Shell_NotifyIcon(NIM_ADD) failed: " + std::to_string(GetLastError()));
        return;
    }
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

// 服务停止、卸载或启动失败后，原先由 service 托管的 TrayApp 已不存在。配置窗口
// 尝试接管同一会话的 Local mutex 并恢复自己的托盘图标，保证“隐藏到托盘”仍可用。
static void RestoreSetupTrayOwnership() {
    if (!g_trayMutex) {
        g_trayMutex = CreateMutexW(nullptr, TRUE, runtime::kTrayMutexName);
        if (!g_trayMutex) {
            log::Warn("setup tray mutex recreation failed: " +
                      std::to_string(GetLastError()));
            return;
        }
        g_ownsTray = GetLastError() != ERROR_ALREADY_EXISTS;
    } else if (!g_ownsTray) {
        const DWORD wait = WaitForSingleObject(g_trayMutex, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            g_ownsTray = true;
        } else if (wait == WAIT_FAILED) {
            log::Warn("setup tray mutex wait failed: " + std::to_string(GetLastError()));
        }
    }
    if (g_ownsTray) {
        AddTrayIcon(g_hwnd);
    }
}

static void SetOperationControlsEnabled(HWND hwnd, BOOL enabled) {
    constexpr int kOperationControls[] = {
        IDC_PW_EDIT, IDC_PW_SAVE, IDC_PORT_EDIT, IDC_QUALITY_COMBO, IDC_SVC_INSTALL, IDC_SVC_UNINSTALL,
        IDC_SVC_START, IDC_SVC_STOP, IDC_RUN_AGENT, IDC_HIDE_TRAY, IDC_EXIT,
    };
    for (const int id : kOperationControls) {
        EnableWindow(GetDlgItem(hwnd, id), enabled);
    }
}

static void FinishAsyncOperation(HWND hwnd) {
    g_operationInProgress = false;
    SetOperationControlsEnabled(hwnd, TRUE);
    UpdateStatus();
}

static void HandleAsyncOperationCompleted(HWND hwnd,
                                          std::unique_ptr<AsyncOperationResult> result) {
    if (!result) {
        return;
    }

    if (result->operation == AsyncOperation::kInstall) {
        if (!result->install.service.ok) {
            FinishAsyncOperation(hwnd);
            ShowServiceError(hwnd, L"安装服务", result->install.service.error);
            return;
        }

        // 只有服务创建成功后才让出配置窗口的托盘所有权；随后启动服务的慢操作继续
        // 放到工作线程，避免阻塞界面。
        g_startAfterInstall = true;
        g_installFirewallError = result->install.firewallError;
        g_operationInProgress = false;
        RelinquishTrayOwnership();
        // ChangeServiceConfig 只修改后续进程的映像路径。更新正在运行的服务时，
        // 必须先停后启，才能真正加载当前目录下的新 exe；新装或已停止的服务
        // 则直接 Start，避免无意义地把停止错误展示给用户。
        const AsyncOperation nextOperation =
            result->install.updatedExistingService && ServiceRunning()
                ? AsyncOperation::kRestart
                : AsyncOperation::kStart;
        StartAsyncOperation(hwnd, nextOperation);
        return;
    }

    const bool startedAfterInstall =
        (result->operation == AsyncOperation::kStart ||
         result->operation == AsyncOperation::kRestart) &&
        g_startAfterInstall;
    const DWORD installFirewallError = g_installFirewallError;
    if (startedAfterInstall) {
        g_startAfterInstall = false;
        g_installFirewallError = ERROR_SUCCESS;
    }

    FinishAsyncOperation(hwnd);
    switch (result->operation) {
    case AsyncOperation::kStart:
        if (!result->service.ok) {
            RestoreSetupTrayOwnership();
            ShowServiceError(hwnd, L"启动服务", result->service.error);
            return;
        }
        {
            std::wstring message = result->agentReady
                ? L"服务与 Agent 已启动并开始监听。浏览器访问 http://<本机IP>:" +
                    std::to_wstring(g_cfg.port) + L"/"
                : L"服务已启动。Agent 尚未就绪（等待登录桌面或查看 logs\\service.log）。";
            if (startedAfterInstall && installFirewallError != ERROR_SUCCESS) {
                message += L"\n\n未能自动添加专用网络防火墙规则；局域网访问可能被阻止。";
            }
            MessageBoxW(hwnd, message.c_str(), L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
        }
        return;
    case AsyncOperation::kStop:
        if (result->service.ok) {
            RestoreSetupTrayOwnership();
            MessageBoxW(hwnd, L"服务已停止。", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
        } else {
            ShowServiceError(hwnd, L"停止服务", result->service.error);
        }
        return;
    case AsyncOperation::kUninstall:
        if (result->service.ok) {
            RestoreSetupTrayOwnership();
            MessageBoxW(hwnd, L"服务已卸载。", L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
        } else {
            ShowServiceError(hwnd, L"卸载服务", result->service.error);
        }
        return;
    case AsyncOperation::kRestart:
        if (!result->service.ok) {
            if (!ServiceRunning()) {
                RestoreSetupTrayOwnership();
            }
            if (startedAfterInstall) {
                ShowServiceError(hwnd, L"更新后的服务重启", result->service.error);
            } else {
                MessageBoxW(hwnd,
                            L"配置已保存，但服务重启失败；新配置会在下次服务启动时生效。",
                            L"RemoteAssist", MB_OK | MB_ICONWARNING);
                ShowServiceError(hwnd, L"重启服务", result->service.error);
            }
            return;
        }
        {
            std::wstring message;
            if (startedAfterInstall) {
                message = result->agentReady
                    ? L"服务已更新并重启，当前版本的 Agent 已开始监听；首帧会在浏览器连接后验证。"
                    : L"服务已更新并重启；Agent 尚未就绪（查看 logs\\service.log）。";
            } else {
                message = result->agentReady
                    ? L"配置已保存，服务与 Agent 已重启并开始监听；首帧会在浏览器连接后验证。"
                    : L"配置已保存，服务已重启；Agent 尚未就绪（查看 logs\\service.log）。";
            }
            const DWORD firewallError = startedAfterInstall
                ? installFirewallError
                : result->firewallError;
            if (firewallError != ERROR_SUCCESS) {
                message += L"\n\n未能自动更新专用网络防火墙规则；局域网访问可能被阻止。";
            }
            MessageBoxW(hwnd, message.c_str(), L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
        }
        return;
    case AsyncOperation::kUpdateFirewall:
        if (result->firewallError != ERROR_SUCCESS) {
            MessageBoxW(hwnd,
                        L"配置已保存，但更新专用网络防火墙规则失败；局域网访问可能被阻止。",
                        L"RemoteAssist", MB_OK | MB_ICONWARNING);
            ShowServiceError(hwnd, L"更新专用网络防火墙规则", result->firewallError);
        } else {
            MessageBoxW(hwnd, L"配置已保存，将在下次启动 Agent 或服务时生效。",
                        L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
        }
        return;
    case AsyncOperation::kInstall:
        return;
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
    if (!g_ownsTray || !g_trayAdded) {
        return;
    }
    wcscpy_s(g_nid.szTip, L"RemoteAssist (hidden)");
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void CreateControls(HWND hwnd) {
    const UINT dpi = WindowDpi(hwnd);
    const auto scale = [dpi](int value) { return ScaleByDpi(value, dpi); };
    // 使用逻辑像素创建 18px 字体；在 100% 缩放下也比原先 16px 字体更易读，并在
    // 高 DPI 显示器上随窗口同步放大。
    g_font = CreateFontW(-scale(18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0,
        L"Microsoft YaHei UI");

    auto mk = [&](int id, const wchar_t* cls, const wchar_t* text, int x, int y, int w, int h, DWORD style) {
        HWND hw = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
            scale(x), scale(y), scale(w), scale(h), hwnd, (HMENU)(LONG_PTR)id, nullptr, nullptr);
        if (g_font) {
            SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
        return hw;
    };

    mk(0, L"STATIC", L"\u5bc6\u7801(\u7559\u7a7a\u4e0d\u53d8):", 28, 28, 150, 28, 0);
    mk(IDC_PW_EDIT, L"EDIT", L"", 180, 24, 190, 38,
       ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER);
    mk(IDC_PW_SAVE, L"BUTTON", L"\u4fdd\u5b58\u914d\u7f6e", 390, 24, 190, 40, BS_PUSHBUTTON);

    mk(0, L"STATIC", L"\u7aef\u53e3:", 28, 88, 72, 28, 0);
    mk(IDC_PORT_EDIT, L"EDIT", L"7980", 100, 84, 110, 38, ES_NUMBER | WS_BORDER);
    mk(0, L"STATIC", L"\u753b\u8d28\u4e0a\u9650:", 232, 88, 120, 28, 0);
    HWND qualityCombo = mk(IDC_QUALITY_COMBO, L"COMBOBOX", L"", 352, 82, 228, 200,
                           CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL);
    SendMessageW(qualityCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"\u81ea\u52a8\uff08\u6700\u9ad81080p\uff09"));
    SendMessageW(qualityCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"\u6700\u9ad81080p"));
    SendMessageW(qualityCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"\u6700\u9ad8720p"));
    SendMessageW(qualityCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"\u6700\u9ad8540p"));
    SendMessageW(qualityCombo, CB_SETCURSEL, 0, 0);

    mk(0, L"STATIC", L"\u72b6\u6001:", 28, 142, 72, 28, 0);
    mk(IDC_STATUS, L"STATIC", L"...", 100, 142, 480, 28, 0);

    mk(IDC_RUN_AGENT, L"BUTTON", L"\u76f4\u63a5\u8fd0\u884c(\u975e\u670d\u52a1)", 28, 190, 270, 50, BS_PUSHBUTTON);
    mk(IDC_SVC_INSTALL, L"BUTTON", L"\u5b89\u88c5/\u66f4\u65b0\u5e76\u542f\u52a8", 312, 190, 268, 50, BS_PUSHBUTTON);

    mk(IDC_SVC_START, L"BUTTON", L"\u542f\u52a8\u670d\u52a1", 28, 260, 170, 50, BS_PUSHBUTTON);
    mk(IDC_SVC_STOP, L"BUTTON", L"\u505c\u6b62\u670d\u52a1", 219, 260, 170, 50, BS_PUSHBUTTON);
    mk(IDC_SVC_UNINSTALL, L"BUTTON", L"\u5378\u8f7d\u670d\u52a1", 410, 260, 170, 50, BS_PUSHBUTTON);

    mk(IDC_HIDE_TRAY, L"BUTTON", L"\u9690\u85cf\u5230\u6258\u76d8", 28, 335, 210, 46, BS_PUSHBUTTON);
    mk(IDC_EXIT, L"BUTTON", L"\u9000\u51fa", 260, 335, 130, 46, BS_PUSHBUTTON);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // TaskbarCreated: Explorer \u91cd\u542f\u540e\u91cd\u5efa\u6258\u76d8\u56fe\u6807
    if (msg == g_wmTaskbarRestart && g_wmTaskbarRestart != 0) {
        if (g_trayAdded) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);
        }
        return 0;
    }

    if (msg == kAsyncOperationCompleted) {
        std::unique_ptr<AsyncOperationResult> result(
            reinterpret_cast<AsyncOperationResult*>(lp));
        HandleAsyncOperationCompleted(hwnd, std::move(result));
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
            const std::wstring password = WideFromUtf8(g_cfg.initialPassword);
            if (!password.empty()) {
                MessageBoxW(hwnd,
                            (L"首次访问密码（请立即保存）：\n\n" + password +
                             L"\n\n安装服务后，浏览器访问 http://<本机IP>:" +
                             std::to_wstring(g_cfg.port) + L"/ 输入此密码连接。")
                                .c_str(),
                            L"RemoteAssist 首次配置", MB_OK | MB_ICONINFORMATION);
            }
            // 密码已交给编辑框和提示框；Config 仅保存哈希，清除内存副本避免在
            // 后续服务操作、日志或异常转储中不必要地延长明文生命周期。
            SecureClearString(g_cfg.initialPassword);
        }
        SetDlgItemTextA(hwnd, IDC_PORT_EDIT, std::to_string(g_cfg.port).c_str());
        SendMessageW(GetDlgItem(hwnd, IDC_QUALITY_COMBO), CB_SETCURSEL,
                     ComboSelectionForQualityCap(g_cfg.qualityCap), 0);
        UpdateStatus();
        // Agent 的 listener-ready/frame-ready 事件会在配置窗口打开后继续变化。
        // 用低频轮询刷新状态，避免用户已连接并收到画面但界面仍停留在旧提示。
        SetTimer(hwnd, kStatusRefreshTimerId, kStatusRefreshIntervalMs, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == kStatusRefreshTimerId && !g_operationInProgress) {
            UpdateStatus();
        }
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
            if (g_operationInProgress) {
                MessageBoxW(hwnd, L"服务操作尚未完成，请稍候。", L"RemoteAssist",
                            MB_OK | MB_ICONINFORMATION);
                break;
            }
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
                StartAsyncOperation(hwnd, AsyncOperation::kRestart, portChanged);
            } else if (portChanged && ServiceExists()) {
                StartAsyncOperation(hwnd, AsyncOperation::kUpdateFirewall);
            } else {
                MessageBoxW(hwnd, L"配置已保存，将在下次启动 Agent 或服务时生效。",
                            L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
                UpdateStatus();
            }
            break;
        }
        case IDC_SVC_INSTALL: {
            bool changed = false;
            bool portChanged = false;
            if (!SaveSettingsFromControls(hwnd, changed, portChanged)) {
                break;
            }
            StartAsyncOperation(hwnd, AsyncOperation::kInstall);
            break;
        }
        case IDC_SVC_UNINSTALL: {
            StartAsyncOperation(hwnd, AsyncOperation::kUninstall);
            break;
        }
        case IDC_SVC_START: {
            RelinquishTrayOwnership();
            StartAsyncOperation(hwnd, AsyncOperation::kStart);
            break;
        }
        case IDC_SVC_STOP: {
            StartAsyncOperation(hwnd, AsyncOperation::kStop);
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
            if (g_operationInProgress) {
                MessageBoxW(hwnd, L"服务操作尚未完成，请稍候。", L"RemoteAssist",
                            MB_OK | MB_ICONINFORMATION);
                break;
            }
            RemoveTrayIcon();
            PostQuitMessage(0);
            break;
        }
        return 0;

    // \u5173\u95ed\u7a97\u53e3(X \u6309\u94ae):\u9690\u85cf\u5230\u6258\u76d8\u800c\u4e0d\u9000\u51fa
    case WM_CLOSE:
        if (g_operationInProgress) {
            MessageBoxW(hwnd, L"服务操作尚未完成，请稍候。", L"RemoteAssist",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (g_ownsTray) {
            HideToTray();
        } else {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kStatusRefreshTimerId);
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

    const UINT initialDpi = WindowDpi(nullptr);
    HWND hwnd = CreateWindowExW(0, cls, L"RemoteAssist \u914d\u7f6e",
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, ScaleByDpi(640, initialDpi), ScaleByDpi(460, initialDpi),
        nullptr, nullptr, hInst, nullptr);

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
