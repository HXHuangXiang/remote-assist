#include <windows.h>

#include <shellapi.h>

#include "agent/Agent.h"
#include "common/Log.h"
#include "common/Path.h"
#include "common/ServiceInstallSecurity.h"
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
    const bool checkServiceInstallPath = HasArg(argc, argv, L"--check-service-install-dir");
    LocalFree(argv);

    if (checkServiceInstallPath) {
        // 供 tools/install.bat 在调用 sc create 前复用与图形安装完全相同的 ACL
        // 校验。此模式不创建窗口，也不写配置或日志。
        return remote_assist::ValidateServiceInstallPath(remote_assist::ModulePath()).secure ? 0 : 2;
    }
    if (agentMode) {
        remote_assist::Agent agent;
        return agent.Run(serviceManaged);
    }
    if (serviceMode) {
        return remote_assist::RunAsService();
    }
    if (trayMode) {
        remote_assist::TrayApp tray;
        return tray.Run();
    }
    // 无参数(用户双击):单实例检查 + 弹出配置窗口,设密码/安装服务。
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"RemoteAssistSetupInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例运行,激活已有窗口后退出。
        HWND hwnd = FindWindowW(L"RemoteAssistSetup", nullptr);
        if (hwnd) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }
    int ret = remote_assist::RunSetupDialog(hInst);
    if (hMutex) CloseHandle(hMutex);
    return ret;
}
