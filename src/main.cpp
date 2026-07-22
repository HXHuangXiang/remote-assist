#include <windows.h>

#include <shellapi.h>
#include <string>

#include "agent/Agent.h"
#include "common/Log.h"
#include "service/ServiceHost.h"
#include "setup/SetupDialog.h"
#include "tray/TrayApp.h"

namespace {

bool HasArg(const std::wstring& cmdLine, const wchar_t* arg) {
    const std::wstring needle(arg);
    return cmdLine.find(needle) != std::wstring::npos;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    const std::wstring cmd = lpCmdLine ? lpCmdLine : L"";

    if (HasArg(cmd, L"--agent")) {
        remote_assist::Agent agent;
        return agent.Run(HasArg(cmd, L"--service-managed"));
    }
    if (HasArg(cmd, L"--service")) {
        return remote_assist::RunAsService();
    }
    if (HasArg(cmd, L"--tray")) {
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
