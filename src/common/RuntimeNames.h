#pragma once

namespace remote_assist::runtime {

// 进程间共享的对象名称。服务、Agent 与配置界面必须使用相同名称。
inline constexpr wchar_t kServiceName[] = L"remote-assist";
inline constexpr wchar_t kAgentMutexName[] = L"Global\\RemoteAssistAgent";
inline constexpr wchar_t kAgentStopEventName[] = L"Global\\RemoteAssistAgentStop";
inline constexpr wchar_t kTrayMutexName[] = L"Local\\RemoteAssistTray";

}  // namespace remote_assist::runtime
