#include <windows.h>

#include <shellapi.h>

#include <string>

#include "agent/Agent.h"
#include "common/Log.h"
#include "common/Path.h"
#include "common/RuntimeNames.h"
#include "service/ServiceHost.h"
#include "setup/SetupDialog.h"
#include "tray/TrayApp.h"

namespace {

// 采集获得的是物理像素，鼠标绝对定位也基于物理虚拟桌面。进程若保持 DPI
// unaware，GetSystemMetrics/GetMonitorInfo 的逻辑坐标会在混合缩放显示器上与
// DXGI 输出脱节，造成单屏选择、鼠标点击和远端指针位置偏移。资源清单已经
// 声明 Per-Monitor V2；此处再在任何窗口/桌面对象创建前动态调用，兼容直接
// 运行、服务拉起和旧安装包缺失资源的情形。
void EnablePerMonitorDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto setContext = user32
        ? reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
        : nullptr;
    if (setContext) {
        // 已由 manifest 设置时 API 会返回 ERROR_ACCESS_DENIED，不能再降级成
        // system-aware；Windows 10+ 的 manifest 结果就是此进程的最终 DPI 模式。
        setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return;
    }
    // 项目最低系统为 Windows 10，正常不会走到这里；保留旧 API 仅供精简/异常
    // user32 环境兜底，至少避免完全 DPI unaware 的坐标虚拟化。
    SetProcessDPIAware();
}

bool HasArg(int argc, wchar_t* const* argv, const wchar_t* target) {
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], target) == 0) {
            return true;
        }
    }
    return false;
}

// 图形配置窗口会在 exe 同级目录写入配置和日志，并负责注册 LocalSystem 服务；无参数
// 入口统一通过 UAC 进入管理员上下文，避免用户在安装阶段切换到另一套操作入口。
// --agent/--tray/--service 均不会走这里，服务拉起的用户 Tray 不会额外弹 UAC。
bool IsCurrentProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &bytes);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

bool RelaunchElevatedSetup() {
    const std::wstring exePath = remote_assist::ModulePath();
    if (exePath.empty()) {
        return false;
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"runas", exePath.c_str(),
                                           L"--setup-elevated", nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

// 仅 Service 生成的随机共享内存名称会通过该参数传入 Tray。不能借用 HasArg，
// 否则释放 CommandLineToArgvW 返回的数组后会留下悬空指针。
std::wstring ArgValue(int argc, wchar_t* const* argv, const wchar_t* target) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (_wcsicmp(argv[index], target) == 0) {
            return argv[index + 1];
        }
    }
    return {};
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    EnablePerMonitorDpiAwareness();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 1;
    }
    const bool agentMode = HasArg(argc, argv, L"--agent");
    const bool serviceMode = HasArg(argc, argv, L"--service");
    const bool trayMode = HasArg(argc, argv, L"--tray");
    const bool serviceManaged = HasArg(argc, argv, L"--service-managed");
    const bool elevatedSetupMode = HasArg(argc, argv, L"--setup-elevated");
    const std::wstring initialPasswordChannel =
        ArgValue(argc, argv, L"--initial-password-channel");
    LocalFree(argv);

    if (agentMode) {
        remote_assist::Agent agent;
        return agent.Run(serviceManaged);
    }
    if (serviceMode) {
        return remote_assist::RunAsService();
    }
    if (trayMode) {
        remote_assist::TrayApp tray;
        return tray.Run(initialPasswordChannel);
    }
    // 无参数(用户双击):先以管理员身份进入配置窗口。用户取消 UAC 时不写配置，
    // 也不会尝试创建或更新服务。
    if (!elevatedSetupMode && !IsCurrentProcessElevated()) {
        if (RelaunchElevatedSetup()) {
            return 0;
        }
        MessageBoxW(nullptr,
                    L"RemoteAssist 需要管理员权限来保存当前目录配置并安装服务。\n"
                    L"请在 UAC 提示中选择“是”，或使用管理员帐户重新启动。",
                    L"RemoteAssist", MB_OK | MB_ICONWARNING);
        return 1;
    }
    if (elevatedSetupMode && !IsCurrentProcessElevated()) {
        MessageBoxW(nullptr, L"配置窗口未获得管理员权限，无法安全继续。",
                    L"RemoteAssist", MB_OK | MB_ICONWARNING);
        return 1;
    }

    // 单实例检查 + 弹出配置窗口,设密码/安装服务。
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, remote_assist::runtime::kSetupMutexName);
    if (!hMutex) {
        MessageBoxW(nullptr,
                    L"无法创建配置窗口的单实例锁，请确认没有被其他进程占用。",
                    L"RemoteAssist", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例运行,激活已有窗口后退出。
        HWND hwnd = FindWindowW(L"RemoteAssistSetup", nullptr);
        if (hwnd) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else {
            // Global mutex 可能由另一个 RDP/控制台会话的配置窗口持有；当前
            // window station 无法跨会话枚举其 HWND，此时明确告知而非静默退出。
            MessageBoxW(nullptr, L"RemoteAssist 配置窗口已在其他会话中打开。",
                        L"RemoteAssist", MB_OK | MB_ICONINFORMATION);
        }
        CloseHandle(hMutex);
        return 0;
    }
    int ret = remote_assist::RunSetupDialog(hInst);
    if (hMutex) CloseHandle(hMutex);
    return ret;
}
