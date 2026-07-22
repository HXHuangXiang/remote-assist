#pragma once

namespace remote_assist::runtime {

// 进程间共享的对象名称。服务、Agent 与配置界面必须使用相同名称。
inline constexpr wchar_t kServiceName[] = L"remote-assist";
inline constexpr wchar_t kAgentMutexName[] = L"Global\\RemoteAssistAgent";
inline constexpr wchar_t kAgentStopEventName[] = L"Global\\RemoteAssistAgentStop";
// 由 service 创建，agent 在 HTTP/WebSocket 成功监听后置位；配置窗口据此区分
// “服务运行中”和“控制端实际可连接”。
inline constexpr wchar_t kAgentReadyEventName[] = L"Global\\RemoteAssistAgentReady";
inline constexpr wchar_t kTrayMutexName[] = L"Local\\RemoteAssistTray";

}  // namespace remote_assist::runtime
