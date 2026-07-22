#include "service/ServiceHost.h"

#include "common/Config.h"
#include "common/Log.h"
#include "common/RuntimeNames.h"
#include "service/ProcessLauncher.h"

#include <windows.h>

#include <atomic>
#include <string>
namespace remote_assist {

namespace {

constexpr DWORD kMonitorIntervalMs = 2000;
// httplib 的 WebSocket 读超时被收敛到 5 秒；为正常的关闭收尾额外预留空间，
// 避免 SCM 停服务时退化为强制终止 agent。
constexpr DWORD kAgentStopTimeoutMs = 8000;
constexpr DWORD kTrayStopTimeoutMs = 1000;

struct ServiceState {
    SERVICE_STATUS status{};
    SERVICE_STATUS_HANDLE hStatus = nullptr;
    std::wstring exePath;
    std::atomic<bool> stopRequested{false};
    HANDLE agentProcess = nullptr;
    HANDLE trayProcess = nullptr;
    HANDLE agentStopEvent = nullptr;
    HANDLE childJob = nullptr;
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
        (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;
    if (g_state.hStatus) {
        SetServiceStatus(g_state.hStatus, &g_state.status);
    }
}

void WINAPI ServiceHandler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP) {
        g_state.stopRequested.store(true);
        ReportState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_state.agentStopEvent) {
            SetEvent(g_state.agentStopEvent);
        }
    }
}

void CloseChild(HANDLE& process, const char* role) {
    if (!process) {
        return;
    }
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(process, &exitCode);
    log::Info(std::string(role) + " exited, code=" + std::to_string(exitCode));
    CloseHandle(process);
    process = nullptr;
}

bool IsChildRunning(HANDLE& process, const char* role) {
    if (!process) {
        return false;
    }
    const DWORD result = WaitForSingleObject(process, 0);
    if (result == WAIT_TIMEOUT) {
        return true;
    }
    if (result == WAIT_OBJECT_0) {
        CloseChild(process, role);
        return false;
    }
    log::Warn(std::string("WaitForSingleObject failed for ") + role +
              ": " + std::to_string(GetLastError()));
    CloseChild(process, role);
    return false;
}

void AddChildToJob(HANDLE process, const char* role) {
    if (!g_state.childJob || !process) {
        return;
    }
    if (!AssignProcessToJobObject(g_state.childJob, process)) {
        log::Warn(std::string("AssignProcessToJobObject failed for ") + role +
                  ": " + std::to_string(GetLastError()));
    }
}

void EnsureAgent() {
    if (IsChildRunning(g_state.agentProcess, "agent")) {
        return;
    }
    if (!g_state.agentStopEvent) {
        return;
    }

    HANDLE process = nullptr;
    if (LaunchAgentInConsoleSession(g_state.exePath, &process)) {
        g_state.agentProcess = process;
        AddChildToJob(process, "agent");
    }
}

void EnsureTray() {
    if (IsChildRunning(g_state.trayProcess, "tray")) {
        return;
    }

    HANDLE process = nullptr;
    if (LaunchTrayInConsoleSession(g_state.exePath, &process)) {
        g_state.trayProcess = process;
        AddChildToJob(process, "tray");
    }
}

void StopChild(HANDLE& process, const char* role, DWORD timeoutMs) {
    if (!process) {
        return;
    }
    if (WaitForSingleObject(process, timeoutMs) == WAIT_TIMEOUT) {
        log::Warn(std::string("force stopping ") + role);
        TerminateProcess(process, ERROR_PROCESS_ABORTED);
        WaitForSingleObject(process, 1000);
    }
    CloseChild(process, role);
}

bool CreateChildJob() {
    // Job 仅由当前服务实例持有，无需暴露为可被其他进程抢占的命名对象。
    g_state.childJob = CreateJobObjectW(nullptr, nullptr);
    if (!g_state.childJob) {
        log::Error("CreateJobObject failed: " + std::to_string(GetLastError()));
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(g_state.childJob,
            JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        log::Error("SetInformationJobObject failed: " + std::to_string(GetLastError()));
        CloseHandle(g_state.childJob);
        g_state.childJob = nullptr;
        return false;
    }
    return true;
}

void ServiceWorker() {
    log::Init(LogDir(), L"service.log");
    log::Info("service worker start");

    // 先报告 RUNNING,避免 SCM 超时报 1053。
    ReportState(SERVICE_RUNNING);

    auto cfg = LoadOrCreateConfig();
    log::Info("config port=" + std::to_string(cfg.port) +
              " fps=" + std::to_string(cfg.fps) +
              " bitrate=" + std::to_string(cfg.bitrate));

    g_state.agentStopEvent = CreateEventW(nullptr, TRUE, FALSE,
                                          runtime::kAgentStopEventName);
    if (!g_state.agentStopEvent) {
        log::Error("CreateEvent(agent stop) failed: " + std::to_string(GetLastError()));
    } else {
        ResetEvent(g_state.agentStopEvent);
    }
    CreateChildJob();

    // 监控循环：未登录时会持续等待会话，子进程异常退出则按下一周期重建。
    while (!g_state.stopRequested.load()) {
        EnsureAgent();
        EnsureTray();
        if (g_state.agentStopEvent) {
            WaitForSingleObject(g_state.agentStopEvent, kMonitorIntervalMs);
        } else {
            Sleep(kMonitorIntervalMs);
        }
    }

    if (g_state.agentStopEvent) {
        SetEvent(g_state.agentStopEvent);
    }
    StopChild(g_state.agentProcess, "agent", kAgentStopTimeoutMs);
    StopChild(g_state.trayProcess, "tray", kTrayStopTimeoutMs);
    if (g_state.childJob) {
        CloseHandle(g_state.childJob);
        g_state.childJob = nullptr;
    }
    if (g_state.agentStopEvent) {
        CloseHandle(g_state.agentStopEvent);
        g_state.agentStopEvent = nullptr;
    }

    log::Info("service worker exit");
    ReportState(SERVICE_STOPPED);
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_state.stopRequested.store(false);
    g_state.hStatus = RegisterServiceCtrlHandlerW(runtime::kServiceName, ServiceHandler);
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
    // StartServiceCtrlDispatcherW 本身失败时 ServiceWorker 尚未运行，也要有
    // 可定位的日志文件。
    log::Init(LogDir(), L"service.log");
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(runtime::kServiceName), ServiceMain },
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
