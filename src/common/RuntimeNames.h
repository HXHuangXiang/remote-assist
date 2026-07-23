#pragma once

namespace remote_assist::runtime {

// 进程间共享的对象名称。服务、Agent 与配置界面必须使用相同名称。
inline constexpr wchar_t kServiceName[] = L"remote-assist";
// 配置窗口也只允许全机一个实例。Agent/Service 使用 Global 对象保证核心进程
// 单例；这里同样跨越 RDP/控制台会话，避免两个管理员同时修改同一份 config.json。
inline constexpr wchar_t kSetupMutexName[] = L"Global\\RemoteAssistSetup";
inline constexpr wchar_t kTrayLogPipeName[] = L"\\\\.\\pipe\\RemoteAssistTrayLog";
inline constexpr wchar_t kAgentMutexName[] = L"Global\\RemoteAssistAgent";
inline constexpr wchar_t kAgentStopEventName[] = L"Global\\RemoteAssistAgentStop";
// 由 service 创建，agent 在 HTTP/WebSocket 成功监听后置位；配置窗口据此区分
// “服务运行中”和“控制端实际可连接”。
inline constexpr wchar_t kAgentReadyEventName[] = L"Global\\RemoteAssistAgentReady";
// 只有成功采集并编码至少一张画面后才置位。与监听就绪分离，避免黑屏时误报
// 画面可用。
inline constexpr wchar_t kAgentFrameReadyEventName[] = L"Global\\RemoteAssistAgentFrameReady";
inline constexpr wchar_t kTrayMutexName[] = L"Local\\RemoteAssistTray";

}  // namespace remote_assist::runtime
