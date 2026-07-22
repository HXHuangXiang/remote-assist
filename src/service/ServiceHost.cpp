#include "service/ServiceHost.h"

#include "common/Config.h"
#include "common/Log.h"
#include "common/Path.h"
#include "common/RuntimeNames.h"
#include "service/ProcessLauncher.h"

#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
namespace remote_assist {

namespace {

constexpr DWORD kMonitorIntervalMs = 2000;
// httplib 的 WebSocket 读超时被收敛到 5 秒；为正常的关闭收尾额外预留空间，
// 避免 SCM 停服务时退化为强制终止 agent。
constexpr DWORD kAgentStopTimeoutMs = 8000;
constexpr DWORD kTrayStopTimeoutMs = 1000;
constexpr DWORD kServiceStopMarginMs = 2000;
constexpr DWORD kChildRetryBaseDelayMs = 1000;
constexpr DWORD kChildRetryMaxDelayMs = 30000;
// Service 创建的全局事件只允许 LocalSystem/管理员修改；交互式用户仅可同步等待
// readyEvent，供配置窗口展示状态。这样普通本地进程不能伪造就绪或随意停止 Agent。
constexpr wchar_t kAgentEventSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;IU)";

struct ChildRetryState {
    DWORD consecutiveFailures = 0;
    ULONGLONG nextAttemptAt = 0;
};

struct ServiceState {
    // SCM 的控制回调线程和 ServiceWorker 都会汇报状态。SERVICE_STATUS 是可变
    // 结构，必须串行更新，避免 stop 控制与工作线程的 checkpoint/wait hint 互相覆盖。
    std::mutex statusMu;
    SERVICE_STATUS status{};
    SERVICE_STATUS_HANDLE hStatus = nullptr;
    std::wstring exePath;
    std::atomic<bool> stopRequested{false};
    HANDLE agentProcess = nullptr;
    HANDLE trayProcess = nullptr;
    HANDLE agentStopEvent = nullptr;
    HANDLE agentReadyEvent = nullptr;
    HANDLE childJob = nullptr;
    DWORD agentSessionId = kInvalidSessionId;
    DWORD traySessionId = kInvalidSessionId;
    bool externalAgentStopRequested = false;
    bool externalTrayDetected = false;
    ChildRetryState agentRetry;
    ChildRetryState trayRetry;
};
ServiceState g_state;

void ReportState(DWORD state, DWORD exitCode = NO_ERROR, DWORD hint = 0,
                 DWORD checkpoint = 0) {
    std::lock_guard<std::mutex> lock(g_state.statusMu);
    g_state.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_state.status.dwCurrentState = state;
    g_state.status.dwWin32ExitCode = exitCode;
    g_state.status.dwServiceSpecificExitCode = 0;
    g_state.status.dwCheckPoint = checkpoint;
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
        // agent 的 WebSocket 关闭窗口最长可达 8 秒，不能继续沿用 5 秒 wait hint，
        // 否则 SCM 或配置窗口可能把仍在优雅退出的服务误判为无响应。
        ReportState(SERVICE_STOP_PENDING, NO_ERROR,
                    kAgentStopTimeoutMs + kTrayStopTimeoutMs + kServiceStopMarginMs, 1);
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

// 子进程因端口占用、显卡驱动异常等原因立即退出时，固定 2 秒轮询会持续创建
// 进程、刷日志并干扰系统恢复。采用 1/2/4/.../30 秒的有界退避，成功启动后清零。
bool IsRetryDue(const ChildRetryState& retry) {
    return GetTickCount64() >= retry.nextAttemptAt;
}

void ResetRetry(ChildRetryState& retry) {
    retry = {};
}

void RecordChildLaunchFailure(ChildRetryState& retry, const char* role) {
    retry.consecutiveFailures = std::min<DWORD>(retry.consecutiveFailures + 1, 6);
    const DWORD multiplier = 1u << (retry.consecutiveFailures - 1);
    const DWORD delayMs = std::min(kChildRetryMaxDelayMs,
        kChildRetryBaseDelayMs * multiplier);
    retry.nextAttemptAt = GetTickCount64() + delayMs;
    log::Warn(std::string(role) + " launch/recovery failed, retry in " +
              std::to_string(delayMs) + "ms (consecutive=" +
              std::to_string(retry.consecutiveFailures) + ")");
}

// Agent/Tray 以“初始所有者”方式持有命名 mutex。服务只有在没有自己管理的
// 子进程时探测它：WAIT_TIMEOUT 代表外部同类进程仍在运行；可立即获取时要先
// ReleaseMutex，避免服务线程意外把对象永久锁住。
bool IsNamedMutexHeld(const wchar_t* name) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    if (!mutex) {
        return false;
    }
    const DWORD result = WaitForSingleObject(mutex, 0);
    if (result == WAIT_TIMEOUT) {
        CloseHandle(mutex);
        return true;
    }
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        ReleaseMutex(mutex);
    } else {
        log::Warn("WaitForSingleObject named mutex failed: " + std::to_string(GetLastError()));
    }
    CloseHandle(mutex);
    return false;
}

void StopChild(HANDLE& process, const char* role, DWORD timeoutMs);

HANDLE CreateProtectedAgentEvent(const wchar_t* name) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kAgentEventSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        log::Error("agent event security descriptor creation failed: " +
                   std::to_string(GetLastError()));
        return nullptr;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    const HANDLE event = CreateEventW(&attributes, TRUE, FALSE, name);
    const DWORD error = event ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!event) {
        log::Error("CreateEvent(agent) failed: " + std::to_string(error));
    }
    return event;
}

void EnsureAgent() {
    if (g_state.stopRequested.load()) {
        return;
    }
    const DWORD targetSessionId = FindActiveInteractiveSessionId();
    const bool hadAgentProcess = g_state.agentProcess != nullptr;
    const bool agentRunning = IsChildRunning(g_state.agentProcess, "agent");
    const bool agentExited = hadAgentProcess && !agentRunning;
    if (agentExited) {
        RecordChildLaunchFailure(g_state.agentRetry, "agent");
    }
    if (agentRunning && (targetSessionId == kInvalidSessionId ||
                         g_state.agentSessionId == targetSessionId)) {
        return;
    }
    if (agentRunning) {
        log::Info("active session changed, restarting agent from session=" +
                  std::to_string(g_state.agentSessionId) + " to session=" +
                  std::to_string(targetSessionId));
        if (g_state.agentStopEvent) {
            SetEvent(g_state.agentStopEvent);
        }
        StopChild(g_state.agentProcess, "agent", kAgentStopTimeoutMs);
    }
    g_state.agentSessionId = kInvalidSessionId;
    if (g_state.agentReadyEvent) {
        ResetEvent(g_state.agentReadyEvent);
    }
    if (targetSessionId == kInvalidSessionId) {
        // 没有交互会话不是子进程故障；用户重新登录后应立即尝试，而不是沿用上次
        // 端口/驱动异常留下的最长退避时间。
        ResetRetry(g_state.agentRetry);
        return;
    }
    if (!g_state.agentStopEvent) {
        return;
    }
    if (IsNamedMutexHeld(runtime::kAgentMutexName)) {
        if (!g_state.externalAgentStopRequested) {
            // 通过公共停止事件接管从配置窗口直接启动的 Agent。它收到事件后会
            // 自行释放键鼠状态并退出，下一轮监控再以 service-managed 模式拉起。
            log::Info("external agent detected, requesting handover to service");
            SetEvent(g_state.agentStopEvent);
            g_state.externalAgentStopRequested = true;
        }
        return;
    }
    g_state.externalAgentStopRequested = false;
    if (!IsRetryDue(g_state.agentRetry)) {
        return;
    }
    ResetEvent(g_state.agentStopEvent);

    HANDLE process = nullptr;
    if (LaunchAgentInConsoleSession(g_state.exePath, &process)) {
        g_state.agentProcess = process;
        DWORD childSessionId = targetSessionId;
        ProcessIdToSessionId(GetProcessId(process), &childSessionId);
        g_state.agentSessionId = childSessionId;
        AddChildToJob(process, "agent");
        ResetRetry(g_state.agentRetry);
    } else {
        // 同一轮中已记录“已启动子进程退出”时，不再把随后的 launch 失败重复
        // 计数；下一次实际重试失败才进入下一档退避。
        if (!agentExited) {
            RecordChildLaunchFailure(g_state.agentRetry, "agent");
        }
    }
}

void EnsureTray() {
    if (g_state.stopRequested.load()) {
        return;
    }
    const DWORD targetSessionId = FindActiveInteractiveSessionId();
    const bool hadTrayProcess = g_state.trayProcess != nullptr;
    const bool trayRunning = IsChildRunning(g_state.trayProcess, "tray");
    const bool trayExited = hadTrayProcess && !trayRunning;
    if (trayExited) {
        RecordChildLaunchFailure(g_state.trayRetry, "tray");
    }
    if (trayRunning && (targetSessionId == kInvalidSessionId ||
                        g_state.traySessionId == targetSessionId)) {
        return;
    }
    if (trayRunning) {
        log::Info("active session changed, restarting tray from session=" +
                  std::to_string(g_state.traySessionId) + " to session=" +
                  std::to_string(targetSessionId));
        StopChild(g_state.trayProcess, "tray", kTrayStopTimeoutMs);
    }
    g_state.traySessionId = kInvalidSessionId;
    if (targetSessionId == kInvalidSessionId) {
        ResetRetry(g_state.trayRetry);
        return;
    }
    if (IsNamedMutexHeld(runtime::kTrayMutexName)) {
        if (!g_state.externalTrayDetected) {
            log::Info("external tray detected, waiting for it to release session ownership");
            g_state.externalTrayDetected = true;
        }
        return;
    }
    g_state.externalTrayDetected = false;
    if (!IsRetryDue(g_state.trayRetry)) {
        return;
    }

    HANDLE process = nullptr;
    if (LaunchTrayInConsoleSession(g_state.exePath, &process)) {
        g_state.trayProcess = process;
        DWORD childSessionId = targetSessionId;
        ProcessIdToSessionId(GetProcessId(process), &childSessionId);
        g_state.traySessionId = childSessionId;
        AddChildToJob(process, "tray");
        ResetRetry(g_state.trayRetry);
    } else {
        if (!trayExited) {
            RecordChildLaunchFailure(g_state.trayRetry, "tray");
        }
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
    if (cfg.passwordHash.empty() || cfg.salt.empty()) {
        log::Error("configuration is invalid; agent will wait for setup dialog repair");
    } else {
        log::Info("config port=" + std::to_string(cfg.port) +
                  " fps=" + std::to_string(cfg.fps) +
                  " bitrate=" + std::to_string(cfg.bitrate));
    }

    g_state.agentStopEvent = CreateProtectedAgentEvent(runtime::kAgentStopEventName);
    if (!g_state.agentStopEvent) {
        log::Error("CreateEvent(agent stop) failed");
    } else {
        ResetEvent(g_state.agentStopEvent);
    }
    g_state.agentReadyEvent = CreateProtectedAgentEvent(runtime::kAgentReadyEventName);
    if (!g_state.agentReadyEvent) {
        log::Warn("CreateEvent(agent ready) failed");
    } else {
        ResetEvent(g_state.agentReadyEvent);
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
    if (g_state.agentReadyEvent) {
        ResetEvent(g_state.agentReadyEvent);
    }
    ReportState(SERVICE_STOP_PENDING, NO_ERROR,
                kAgentStopTimeoutMs + kTrayStopTimeoutMs + kServiceStopMarginMs, 2);
    StopChild(g_state.agentProcess, "agent", kAgentStopTimeoutMs);
    ReportState(SERVICE_STOP_PENDING, NO_ERROR, kTrayStopTimeoutMs + kServiceStopMarginMs, 3);
    StopChild(g_state.trayProcess, "tray", kTrayStopTimeoutMs);
    g_state.agentSessionId = kInvalidSessionId;
    g_state.traySessionId = kInvalidSessionId;
    g_state.externalAgentStopRequested = false;
    g_state.externalTrayDetected = false;
    ResetRetry(g_state.agentRetry);
    ResetRetry(g_state.trayRetry);
    if (g_state.childJob) {
        CloseHandle(g_state.childJob);
        g_state.childJob = nullptr;
    }
    if (g_state.agentStopEvent) {
        CloseHandle(g_state.agentStopEvent);
        g_state.agentStopEvent = nullptr;
    }
    if (g_state.agentReadyEvent) {
        CloseHandle(g_state.agentReadyEvent);
        g_state.agentReadyEvent = nullptr;
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

    g_state.exePath = ModulePath();
    if (g_state.exePath.empty()) {
        log::Error("service cannot resolve executable path: " + std::to_string(GetLastError()));
        ReportState(SERVICE_STOPPED, ERROR_PATH_NOT_FOUND);
        return;
    }

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
