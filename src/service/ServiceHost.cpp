#include "service/ServiceHost.h"

#include "common/Config.h"
#include "common/Log.h"
#include "service/ProcessLauncher.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace remote_assist {

namespace {

constexpr const wchar_t* kServiceName = L"RemoteAssist";

struct ServiceState {
    SERVICE_STATUS status{};
    SERVICE_STATUS_HANDLE hStatus = nullptr;
    std::wstring exePath;
    std::atomic<bool> stopRequested{false};
};
ServiceState g_state;

void ReportState(DWORD state, DWORD exitCode = NO_ERROR, DWORD hint = 0) {
    g_state.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_state.status.dwCurrentState = state;
    g_state.status.dwWin32ExitCode = exitCode;
    g_state.status.dwServiceSpecificExitCode = 0;
    g_state.status.dwCheckPoint = 0;
    g_state.status.dwWaitHint = hint;
    g_state.status.dwControlsAccepted =
        (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP;
    if (g_state.hStatus) {
        SetServiceStatus(g_state.hStatus, &g_state.status);
    }
}

void WINAPI ServiceHandler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP) {
        g_state.stopRequested.store(true);
        ReportState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
    }
}

void ServiceWorker() {
    log::Init(LogDir());
    log::Info("service worker start");

    auto cfg = LoadOrCreateConfig();
    log::Info("config port=" + std::to_string(cfg.port) +
              " fps=" + std::to_string(cfg.fps) +
              " bitrate=" + std::to_string(cfg.bitrate));

    // 启动 agent 到 winlogon 桌面;启动 tray 到用户桌面。
    // agent 内部会重新 OpenInputDesktop 跟随当前桌面(锁屏/解锁切换)。
    LaunchAgentInConsoleSession(g_state.exePath);
    LaunchTrayInConsoleSession(g_state.exePath);

    ReportState(SERVICE_RUNNING);

    // MVP 监控循环:每 5 秒检查停止请求;agent 自然死亡由后续迭代加重启。
    while (!g_state.stopRequested.load()) {
        Sleep(5000);
    }

    log::Info("service worker exit");
    ReportState(SERVICE_STOPPED);
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_state.hStatus = RegisterServiceCtrlHandlerW(kServiceName, ServiceHandler);
    if (!g_state.hStatus) {
        return;
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_state.exePath = exePath;

    ReportState(SERVICE_START_PENDING, NO_ERROR, 3000);
    ServiceWorker();
}

}  // namespace

int RunAsService() {
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        log::Error("StartServiceCtrlDispatcherW failed: " +
                   std::to_string(GetLastError()));
        return 1;
    }
    return 0;
}

}  // namespace remote_assist

