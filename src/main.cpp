#include <windows.h>

#include <shellapi.h>

#include "agent/Agent.h"
#include "common/Log.h"
#include "service/ServiceHost.h"
#include "setup/SetupDialog.h"
#include "tray/TrayApp.h"

namespace {

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
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 1;
    }
    const bool agentMode = HasArg(argc, argv, L"--agent");
    const bool serviceMode = HasArg(argc, argv, L"--service");
    const bool trayMode = HasArg(argc, argv, L"--tray");
    const bool serviceManaged = HasArg(argc, argv, L"--service-managed");
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
