#include "service/ServiceHost.h"

#include "common/Config.h"
#include "common/InitialPasswordChannel.h"
#include "common/Log.h"
#include "common/Path.h"
#include "common/RuntimeNames.h"
#include "service/ProcessLauncher.h"

#include <windows.h>
#include <aclapi.h>
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
// Tray 启动后应立即读取共享内存；保留 30 秒兼顾首次登录时 Explorer 的启动延迟，
// 到期后擦除映射，避免明文长期保留在 Service 地址空间。
constexpr ULONGLONG kTrayInitialPasswordChannelLifetimeMs = 30'000;
constexpr DWORD kTrayLogPipeBufferBytes = 8 * 1024;
// 日志管道采用短连接；100ms 轮询既能让 Tray 的启动和菜单操作诊断及时落盘，
// 又不会把子进程会话/存活检查从原来的 2 秒频率放大十几倍。
constexpr DWORD kTrayLogPipePollIntervalMs = 100;
// Service 创建的全局事件只允许 LocalSystem/管理员修改；交互式用户仅可同步等待
// readyEvent，供配置窗口展示状态。这样普通本地进程不能伪造就绪或随意停止 Agent。
constexpr wchar_t kAgentObjectSddl[] =
    L"O:SYG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;IU)";

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
    // SCM 控制回调可与 ServiceWorker 同时执行。停止事件句柄关闭前必须与回调
    // 串行化，避免 SetEvent 和 CloseHandle 并发作用于同一个原始 HANDLE。
    std::mutex agentStopEventMu;
    HANDLE agentProcess = nullptr;
    HANDLE trayProcess = nullptr;
    HANDLE agentStopEvent = nullptr;
    HANDLE agentReadyEvent = nullptr;
    HANDLE agentFrameReadyEvent = nullptr;
    HANDLE agentMutex = nullptr;
    HANDLE childJob = nullptr;
    DWORD agentSessionId = kInvalidSessionId;
    DWORD traySessionId = kInvalidSessionId;
    bool externalAgentStopRequested = false;
    bool externalTrayDetected = false;
    ChildRetryState agentRetry;
    ChildRetryState trayRetry;
    std::string pendingTrayInitialPassword;
    InitialPasswordChannel trayInitialPasswordChannel;
    ULONGLONG trayInitialPasswordChannelExpiresAt = 0;
    // Tray 运行在普通用户令牌下，日志由 Service 以 LocalSystem 权限落盘。管道
    // DACL 仅授权当前活动会话用户写入，避免为 tray.log 放宽安装目录 ACL。
    HANDLE trayLogPipe = nullptr;
    bool trayLogPipeConnected = false;
    std::wstring trayLogPipeUserSid;
};
ServiceState g_state;

// 不直接从 SCM 回调读取 g_state.agentStopEvent。关闭路径会先在同一把锁内摘除
// 句柄，因而 SetEvent 完成前 CloseHandle 不会发生；摘除后则把本次控制请求安全地
// 视为“运行时对象已结束”。
bool SignalAgentStopEvent() {
    std::lock_guard<std::mutex> lock(g_state.agentStopEventMu);
    return g_state.agentStopEvent && SetEvent(g_state.agentStopEvent) != FALSE;
}

// 创建 Agent 前重新置为无信号。必须把 stopRequested 的检查和 ResetEvent 放在
// 同一临界区：否则 STOP 控制可能刚 SetEvent，又被启动路径重置，导致刚拉起的
// Agent 错过退出信号。
bool ResetAgentStopEventForLaunch() {
    std::lock_guard<std::mutex> lock(g_state.agentStopEventMu);
    return !g_state.stopRequested.load() && g_state.agentStopEvent &&
        ResetEvent(g_state.agentStopEvent) != FALSE;
}

void SecureClearString(std::string& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

void SecureClearWideString(std::wstring& value) {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
    }
}

void ExpireTrayInitialPasswordChannel() {
    if (!g_state.trayInitialPasswordChannel.IsOpen() ||
        GetTickCount64() < g_state.trayInitialPasswordChannelExpiresAt) {
        return;
    }
    g_state.trayInitialPasswordChannel.Close();
    g_state.trayInitialPasswordChannelExpiresAt = 0;
    log::Info("initial password channel expired and was cleared");
}

void CloseTrayInitialPasswordChannel() {
    g_state.trayInitialPasswordChannel.Close();
    g_state.trayInitialPasswordChannelExpiresAt = 0;
}

void CloseTrayLogPipe() {
    if (g_state.trayLogPipe) {
        if (g_state.trayLogPipeConnected) {
            DisconnectNamedPipe(g_state.trayLogPipe);
        }
        CloseHandle(g_state.trayLogPipe);
        g_state.trayLogPipe = nullptr;
    }
    g_state.trayLogPipeConnected = false;
    SecureClearWideString(g_state.trayLogPipeUserSid);
}

// PIPE_NOWAIT 下 ConnectNamedPipe 不会等待客户端，ERROR_PIPE_LISTENING 表示实例
// 已进入可接受客户端的状态。创建后立即调用，确保 Tray 的第一条“starting”日志
// 不会因为服务还没走到下一轮监控而丢失。
bool BeginTrayLogPipeConnection() {
    if (!g_state.trayLogPipe || g_state.trayLogPipeConnected) {
        return g_state.trayLogPipe != nullptr;
    }
    if (ConnectNamedPipe(g_state.trayLogPipe, nullptr)) {
        g_state.trayLogPipeConnected = true;
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        g_state.trayLogPipeConnected = true;
        return true;
    }
    if (error == ERROR_PIPE_LISTENING) {
        return true;
    }
    if (error == ERROR_NO_DATA) {
        // 客户端在服务开始接受前已经关闭；回到断开状态，下一轮即可重新监听。
        DisconnectNamedPipe(g_state.trayLogPipe);
        return true;
    }
    log::Warn("ConnectNamedPipe(tray log) failed: " + std::to_string(error));
    DisconnectNamedPipe(g_state.trayLogPipe);
    return false;
}

bool CreateTrayLogPipeForUser(const std::wstring& userSid) {
    CloseTrayLogPipe();
    PSID parsedSid = nullptr;
    if (userSid.empty() || !ConvertStringSidToSidW(userSid.c_str(), &parsedSid)) {
        log::Warn("tray log pipe user SID invalid: " + std::to_string(GetLastError()));
        return false;
    }
    LocalFree(parsedSid);

    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GW;;;" + userSid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        log::Warn("tray log pipe security descriptor failed: " +
                  std::to_string(GetLastError()));
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    const HANDLE pipe = CreateNamedPipeW(
        runtime::kTrayLogPipeName, PIPE_ACCESS_INBOUND,
        // Tray 日志只应来自本机当前交互用户；服务无需接受远程命名管道客户端。
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 0, kTrayLogPipeBufferBytes, 0, &attributes);
    const DWORD error = pipe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        log::Warn("CreateNamedPipe(tray log) failed: " + std::to_string(error));
        return false;
    }
    g_state.trayLogPipe = pipe;
    g_state.trayLogPipeUserSid = userSid;
    if (!BeginTrayLogPipeConnection()) {
        CloseTrayLogPipe();
        return false;
    }
    return true;
}

bool EnsureTrayLogPipe(DWORD sessionId) {
    std::wstring userSid;
    if (!GetSessionUserSidString(sessionId, userSid)) {
        log::Warn("tray log pipe user SID unavailable");
        return false;
    }
    const bool ready = g_state.trayLogPipe &&
        g_state.trayLogPipeUserSid == userSid;
    if (!ready) {
        const bool created = CreateTrayLogPipeForUser(userSid);
        SecureClearWideString(userSid);
        return created;
    }
    SecureClearWideString(userSid);
    return true;
}

void PumpTrayLogPipe() {
    if (!g_state.trayLogPipe) {
        return;
    }
    if (!BeginTrayLogPipeConnection() || !g_state.trayLogPipeConnected) {
        return;
    }

    char payload[kTrayLogPipeBufferBytes] = {};
    DWORD bytesRead = 0;
    if (!ReadFile(g_state.trayLogPipe, payload, sizeof(payload), &bytesRead, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NO_DATA) {
            return;
        }
        if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) {
            log::Warn("ReadFile(tray log) failed: " + std::to_string(error));
        }
        DisconnectNamedPipe(g_state.trayLogPipe);
        g_state.trayLogPipeConnected = false;
        BeginTrayLogPipeConnection();
        return;
    }

    if (bytesRead != 0) {
        std::string message(payload, payload + bytesRead);
        for (char& ch : message) {
            if (ch == '\r' || ch == '\n' || ch == '\0') {
                ch = ' ';
            }
        }
        log::WriteToFile(L"tray.log", "INFO", "tray: " + message);
    }
    DisconnectNamedPipe(g_state.trayLogPipe);
    g_state.trayLogPipeConnected = false;
    BeginTrayLogPipeConnection();
}

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
        SignalAgentStopEvent();
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

// Kill-on-close Job 是 Service 异常退出时回收 Agent/Tray 的最后一道保障。子进程
// 不能成功加入 Job 时不能继续当作已受管进程运行，否则会留下脱离服务生命周期的实例。
bool AddChildToJob(HANDLE process, const char* role) {
    if (!g_state.childJob || !process) {
        SetLastError(ERROR_INVALID_HANDLE);
        log::Error(std::string("unable to assign ") + role + " to child job: invalid handle");
        return false;
    }
    if (AssignProcessToJobObject(g_state.childJob, process)) {
        return true;
    }
    log::Error(std::string("AssignProcessToJobObject failed for ") + role +
               ": " + std::to_string(GetLastError()));
    return false;
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

bool ApplyProtectedAgentObjectSecurity(HANDLE object, PSECURITY_DESCRIPTOR descriptor,
                                       const char* objectType) {
    if (SetKernelObjectSecurity(object, OWNER_SECURITY_INFORMATION |
                                GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                descriptor)) {
        return true;
    }
    log::Error(std::string("failed to restore protected ACL for ") + objectType + ": " +
               std::to_string(GetLastError()));
    return false;
}

PSECURITY_DESCRIPTOR CreateProtectedAgentDescriptor() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kAgentObjectSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        log::Error("agent object security descriptor creation failed: " +
                   std::to_string(GetLastError()));
        return nullptr;
    }
    return descriptor;
}

HANDLE CreateProtectedAgentEvent(const wchar_t* name) {
    PSECURITY_DESCRIPTOR descriptor = CreateProtectedAgentDescriptor();
    if (!descriptor) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    SetLastError(ERROR_SUCCESS);
    const HANDLE event = CreateEventW(&attributes, TRUE, FALSE, name);
    const DWORD error = GetLastError();
    const bool existed = event && error == ERROR_ALREADY_EXISTS;
    const bool secured = !event || !existed ||
        ApplyProtectedAgentObjectSecurity(event, descriptor, "event");
    LocalFree(descriptor);
    if (!event || !secured) {
        // 已存在对象的 ACL 修复失败时，真正原因来自 SetKernelObjectSecurity，不能
        // 误把 ERROR_ALREADY_EXISTS 回传给 SCM 和配置窗口。
        const DWORD failure = event ? GetLastError() : error;
        log::Error("CreateEvent(agent) failed: " + std::to_string(failure));
        if (event) {
            CloseHandle(event);
        }
        SetLastError(failure);
        return nullptr;
    }
    return event;
}

HANDLE CreateProtectedAgentMutex(const wchar_t* name) {
    PSECURITY_DESCRIPTOR descriptor = CreateProtectedAgentDescriptor();
    if (!descriptor) {
        return nullptr;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    SetLastError(ERROR_SUCCESS);
    const HANDLE mutex = CreateMutexW(&attributes, FALSE, name);
    const DWORD error = GetLastError();
    const bool existed = mutex && error == ERROR_ALREADY_EXISTS;
    const bool secured = !mutex || !existed ||
        ApplyProtectedAgentObjectSecurity(mutex, descriptor, "mutex");
    LocalFree(descriptor);
    if (!mutex || !secured) {
        const DWORD failure = mutex ? GetLastError() : error;
        log::Error("CreateMutex(agent) failed: " + std::to_string(failure));
        if (mutex) {
            CloseHandle(mutex);
        }
        SetLastError(failure);
        return nullptr;
    }
    return mutex;
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
        SignalAgentStopEvent();
        StopChild(g_state.agentProcess, "agent", kAgentStopTimeoutMs);
    }
    g_state.agentSessionId = kInvalidSessionId;
    if (g_state.agentReadyEvent) {
        ResetEvent(g_state.agentReadyEvent);
    }
    if (g_state.agentFrameReadyEvent) {
        ResetEvent(g_state.agentFrameReadyEvent);
    }
    if (targetSessionId == kInvalidSessionId) {
        // 没有交互会话不是子进程故障；用户重新登录后应立即尝试，而不是沿用上次
        // 端口/驱动异常留下的最长退避时间。
        ResetRetry(g_state.agentRetry);
        return;
    }
    if (!g_state.agentMutex) {
        return;
    }
    if (IsNamedMutexHeld(runtime::kAgentMutexName)) {
        if (!g_state.externalAgentStopRequested) {
            // 通过公共停止事件接管从配置窗口直接启动的 Agent。它收到事件后会
            // 自行释放键鼠状态并退出，下一轮监控再以 service-managed 模式拉起。
            log::Info("external agent detected, requesting handover to service");
            SignalAgentStopEvent();
            g_state.externalAgentStopRequested = true;
        }
        return;
    }
    g_state.externalAgentStopRequested = false;
    if (!IsRetryDue(g_state.agentRetry)) {
        return;
    }
    if (!ResetAgentStopEventForLaunch()) {
        return;
    }

    HANDLE process = nullptr;
    if (LaunchAgentInConsoleSession(g_state.exePath, &process)) {
        if (!AddChildToJob(process, "agent")) {
            // CreateProcessAsUser 已返回的句柄仍可用于优雅停止/强制回收。绝不能
            // 把未受 Job 约束的 LocalSystem Agent 留给后续服务崩溃或卸载流程。
            SignalAgentStopEvent();
            StopChild(process, "unmanaged agent", kAgentStopTimeoutMs);
            RecordChildLaunchFailure(g_state.agentRetry, "agent");
            return;
        }
        g_state.agentProcess = process;
        DWORD childSessionId = targetSessionId;
        ProcessIdToSessionId(GetProcessId(process), &childSessionId);
        g_state.agentSessionId = childSessionId;
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
    ExpireTrayInitialPasswordChannel();
    const DWORD targetSessionId = FindActiveInteractiveSessionId();
    if (targetSessionId == kInvalidSessionId) {
        // 目标用户已注销时，旧用户 SID 的管道不应继续保留。下一次登录会按新
        // 会话 SID 重建，避免把新 Tray 的诊断落入不匹配的授权端点。
        CloseTrayLogPipe();
        ResetRetry(g_state.trayRetry);
        return;
    }
    // 先建立受限管道，再启动 Tray。Tray 在启动早期就会记录状态，若后置创建，
    // 那些最有价值的启动失败日志会因安装目录不可写而只能留在调试输出。
    if (!EnsureTrayLogPipe(targetSessionId)) {
        log::Warn("tray log pipe unavailable; tray diagnostics stay in debug output");
    }
    const bool hadTrayProcess = g_state.trayProcess != nullptr;
    const bool trayRunning = IsChildRunning(g_state.trayProcess, "tray");
    const bool trayExited = hadTrayProcess && !trayRunning;
    if (trayExited) {
        CloseTrayInitialPasswordChannel();
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

    std::wstring initialPasswordChannel;
    bool passwordChannelCreated = false;
    if (!g_state.pendingTrayInitialPassword.empty()) {
        CloseTrayInitialPasswordChannel();
        std::wstring trayUserSid;
        if (GetSessionUserSidString(targetSessionId, trayUserSid)) {
            passwordChannelCreated = g_state.trayInitialPasswordChannel.Create(
                g_state.pendingTrayInitialPassword, trayUserSid, initialPasswordChannel);
            SecureZeroMemory(trayUserSid.data(), trayUserSid.size() * sizeof(wchar_t));
        } else {
            log::Warn("tray user SID unavailable; skip first-password delivery channel");
        }
        if (!passwordChannelCreated) {
            // Create 失败后不应把部分生成的对象名带入命令行；Close 也会擦除已写入
            // 的映射内容。Tray 仍可正常启动，只是不会展示首次随机密码。
            CloseTrayInitialPasswordChannel();
            initialPasswordChannel.clear();
            log::Warn("tray will start without first-password delivery channel");
        }
    }

    HANDLE process = nullptr;
    if (LaunchTrayInSession(g_state.exePath, targetSessionId, initialPasswordChannel, &process)) {
        if (!AddChildToJob(process, "tray")) {
            // Tray 命令行可能携带一次性密码通道名称；回收未受管进程前先关闭映射，
            // 防止服务异常退出时留下可读取的明文窗口。
            CloseTrayInitialPasswordChannel();
            StopChild(process, "unmanaged tray", kTrayStopTimeoutMs);
            RecordChildLaunchFailure(g_state.trayRetry, "tray");
            return;
        }
        g_state.trayProcess = process;
        DWORD childSessionId = targetSessionId;
        ProcessIdToSessionId(GetProcessId(process), &childSessionId);
        g_state.traySessionId = childSessionId;
        ResetRetry(g_state.trayRetry);
        if (passwordChannelCreated) {
            g_state.trayInitialPasswordChannelExpiresAt = GetTickCount64() +
                kTrayInitialPasswordChannelLifetimeMs;
        }
        // Tray 已成功接管该会话后，不能因一次性通道创建失败把明文长期留在
        // LocalSystem 服务内存中。失败时用户仍可从配置窗口设置新密码。
        if (!g_state.pendingTrayInitialPassword.empty()) {
            if (!passwordChannelCreated) {
                log::Warn("first password delivery unavailable; clear pending plaintext");
            }
            SecureClearString(g_state.pendingTrayInitialPassword);
        }
    } else {
        if (passwordChannelCreated) {
            CloseTrayInitialPasswordChannel();
        }
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

// Agent mutex、停止事件和 Job 是受管运行的基础：缺少其中任一项时，服务即使
// 仍留在 SCM 的 RUNNING 状态也无法可靠拉起或回收高权限子进程。集中释放可
// 保证启动中途失败与正常停止采用同一顺序，避免残留命名对象误导下一次启动。
void CloseAgentRuntimeObjects() {
    if (g_state.childJob) {
        CloseHandle(g_state.childJob);
        g_state.childJob = nullptr;
    }
    HANDLE agentStopEvent = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state.agentStopEventMu);
        agentStopEvent = g_state.agentStopEvent;
        g_state.agentStopEvent = nullptr;
    }
    if (agentStopEvent) {
        CloseHandle(agentStopEvent);
    }
    if (g_state.agentReadyEvent) {
        CloseHandle(g_state.agentReadyEvent);
        g_state.agentReadyEvent = nullptr;
    }
    if (g_state.agentFrameReadyEvent) {
        CloseHandle(g_state.agentFrameReadyEvent);
        g_state.agentFrameReadyEvent = nullptr;
    }
    if (g_state.agentMutex) {
        CloseHandle(g_state.agentMutex);
        g_state.agentMutex = nullptr;
    }
}

void ServiceWorker() {
    log::Init(LogDir(), L"service.log");
    log::Info("service worker start");

    // 不要在配置和受管对象尚未就绪时上报 RUNNING。否则配置损坏、全局对象 ACL
    // 冲突等故障只会在界面里表现为“等待 Agent”，而 SCM 也无法给出失败码。
    ReportState(SERVICE_START_PENDING, NO_ERROR, 15'000, 2);
    const auto failStartup = [](DWORD error, const char* phase) {
        const DWORD code = error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
        log::Error(std::string("service startup failed at ") + phase + ", error=" +
                   std::to_string(code));
        CloseTrayInitialPasswordChannel();
        CloseTrayLogPipe();
        SecureClearString(g_state.pendingTrayInitialPassword);
        CloseAgentRuntimeObjects();
        ReportState(SERVICE_STOPPED, code);
    };

    auto cfg = LoadOrCreateConfig();
    if (cfg.passwordHash.empty() || cfg.salt.empty()) {
        log::Error("configuration is invalid; repair it in the setup dialog before starting service");
        failStartup(ERROR_INVALID_DATA, "configuration");
        return;
    }
    log::Info("config port=" + std::to_string(cfg.port) +
              " fps=" + std::to_string(cfg.fps) +
              " bitrate=" + std::to_string(cfg.bitrate));
    if (!cfg.initialPassword.empty()) {
        g_state.pendingTrayInitialPassword = cfg.initialPassword;
        SecureClearString(cfg.initialPassword);
        log::Info("first password retained for one-time tray delivery");
    }

    // 先创建并保留 Agent 单实例 mutex，再创建停止/状态事件。受管 Agent 只能打开
    // 这个由 Service 固定 ACL 的对象并取得所有权，避免它在未知全局对象上运行。
    g_state.agentMutex = CreateProtectedAgentMutex(runtime::kAgentMutexName);
    if (!g_state.agentMutex) {
        failStartup(GetLastError(), "agent mutex");
        return;
    }
    HANDLE agentStopEvent = CreateProtectedAgentEvent(runtime::kAgentStopEventName);
    if (!agentStopEvent) {
        failStartup(GetLastError(), "agent stop event");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_state.agentStopEventMu);
        g_state.agentStopEvent = agentStopEvent;
    }
    ResetAgentStopEventForLaunch();
    g_state.agentReadyEvent = CreateProtectedAgentEvent(runtime::kAgentReadyEventName);
    if (!g_state.agentReadyEvent) {
        log::Warn("CreateEvent(agent ready) failed");
    } else {
        ResetEvent(g_state.agentReadyEvent);
    }
    g_state.agentFrameReadyEvent = CreateProtectedAgentEvent(
        runtime::kAgentFrameReadyEventName);
    if (!g_state.agentFrameReadyEvent) {
        log::Warn("CreateEvent(agent frame ready) failed");
    } else {
        ResetEvent(g_state.agentFrameReadyEvent);
    }
    if (!CreateChildJob()) {
        failStartup(GetLastError(), "child job");
        return;
    }
    if (g_state.stopRequested.load()) {
        failStartup(ERROR_CANCELLED, "startup cancelled");
        return;
    }

    // 至此才允许控制台、配置窗口和 SCM 把服务视为可用。Agent 本身仍会在后续
    // 就绪事件中单独报告监听状态，避免把“服务运行”误解为“画面已经可用”。
    ReportState(SERVICE_RUNNING);

    // 子进程监控保持原来的 2 秒节奏；日志管道单独用短等待轮询，避免一个 Tray
    // 短连接占用实例时让后续诊断在整整一个监控周期内持续丢失。
    ULONGLONG nextChildMonitorAt = 0;
    while (!g_state.stopRequested.load()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= nextChildMonitorAt) {
            EnsureAgent();
            EnsureTray();
            nextChildMonitorAt = GetTickCount64() + kMonitorIntervalMs;
        }
        PumpTrayLogPipe();
        const ULONGLONG waitStartedAt = GetTickCount64();
        const ULONGLONG waitUntil = nextChildMonitorAt > waitStartedAt
            ? nextChildMonitorAt - waitStartedAt : 0;
        const DWORD waitMs = static_cast<DWORD>(std::min<ULONGLONG>(
            kTrayLogPipePollIntervalMs, waitUntil));
        if (g_state.agentStopEvent) {
            const DWORD waitResult = WaitForSingleObject(g_state.agentStopEvent, waitMs);
            // 接管独立运行的 Agent 时该手动重置事件会一直保持有信号，直到旧
            // Agent 退出并由下一轮 EnsureAgent 重置。日志管道短轮询时不需要
            // 再额外 Sleep 一个完整监控周期，否则会拖慢所有 Tray 诊断落盘。
            if (waitResult == WAIT_OBJECT_0 && !g_state.stopRequested.load()) {
                Sleep(kTrayLogPipePollIntervalMs);
            }
        } else {
            Sleep(waitMs);
        }
    }

    SignalAgentStopEvent();
    if (g_state.agentReadyEvent) {
        ResetEvent(g_state.agentReadyEvent);
    }
    if (g_state.agentFrameReadyEvent) {
        ResetEvent(g_state.agentFrameReadyEvent);
    }
    ReportState(SERVICE_STOP_PENDING, NO_ERROR,
                kAgentStopTimeoutMs + kTrayStopTimeoutMs + kServiceStopMarginMs, 2);
    StopChild(g_state.agentProcess, "agent", kAgentStopTimeoutMs);
    ReportState(SERVICE_STOP_PENDING, NO_ERROR, kTrayStopTimeoutMs + kServiceStopMarginMs, 3);
    StopChild(g_state.trayProcess, "tray", kTrayStopTimeoutMs);
    CloseTrayInitialPasswordChannel();
    CloseTrayLogPipe();
    SecureClearString(g_state.pendingTrayInitialPassword);
    g_state.agentSessionId = kInvalidSessionId;
    g_state.traySessionId = kInvalidSessionId;
    g_state.externalAgentStopRequested = false;
    g_state.externalTrayDetected = false;
    ResetRetry(g_state.agentRetry);
    ResetRetry(g_state.trayRetry);
    CloseAgentRuntimeObjects();

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
