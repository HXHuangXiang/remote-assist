#include <windows.h>

#include <shellapi.h>
#include <string>

#include "agent/Agent.h"
#include "common/Log.h"
#include "service/ServiceHost.h"
#include "tray/TrayApp.h"

namespace {

bool HasArg(const std::wstring& cmdLine, const wchar_t* arg) {
    const std::wstring needle(arg);
    return cmdLine.find(needle) != std::wstring::npos;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    const std::wstring cmd = lpCmdLine ? lpCmdLine : L"";

    if (HasArg(cmd, L"--agent")) {
        remote_assist::Agent agent;
        return agent.Run();
    }
    if (HasArg(cmd, L"--service")) {
        return remote_assist::RunAsService();
    }
    if (HasArg(cmd, L"--tray")) {
        remote_assist::TrayApp tray;
        return tray.Run();
    }
    // 无参数(用户双击):直接进入托盘。service 需通过 sc 安装为 LocalSystem 后才会拉起 agent。
    remote_assist::TrayApp tray;
    return tray.Run();
}

