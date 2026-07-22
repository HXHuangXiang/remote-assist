#pragma once

#include <windows.h>

#include <string>

namespace remote_assist {

// 在指定会话内查找进程名(如 winlogon.exe / explorer.exe)的 PID;找不到返回 0。
DWORD FindProcessInSession(const wchar_t* exeName, DWORD sessionId);

// 返回当前实际可交互的会话。优先活动控制台，会话桌面型系统中回退到任一 WTSActive
// 会话，因而兼容 RDP 登录；没有可用会话时返回 WTS_INVALID_SESSION_ID。
DWORD FindActiveInteractiveSessionId();

// 复制 srcPid 的 token,以 TokenPrimary + SecurityIdentification 复制成主令牌,
// 然后用 CreateProcessAsUserW 启动 commandLine,桌面设为 desktop。
// 用于让 LocalSystem 服务把子进程送入交互式会话(含 Winlogon 桌面)。
// 成功时由 processOut 接管进程句柄;调用方负责 CloseHandle。
bool LaunchChildWithProcessToken(DWORD srcPid, const std::wstring& commandLine,
                                 const std::wstring& desktop, HANDLE* processOut = nullptr);

// 在当前活动交互会话里,以 winlogon.exe 的 token 启动本 exe 的 --agent 模式,
// 桌面 winsta0\default。agent 进而在 agent 模式内 OpenInputDesktop 切到当前桌面。
bool LaunchAgentInConsoleSession(const std::wstring& exePath, HANDLE* processOut = nullptr);

// 在当前活动交互会话里,以 explorer.exe 的 token 启动 --tray 模式到用户桌面。
bool LaunchTrayInConsoleSession(const std::wstring& exePath, HANDLE* processOut = nullptr);

}  // namespace remote_assist
