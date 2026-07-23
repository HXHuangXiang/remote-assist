#pragma once

#include <windows.h>

#include <string>

namespace remote_assist {

inline constexpr DWORD kInvalidSessionId = 0xFFFFFFFF;

// 在指定会话内查找进程名(如 winlogon.exe / explorer.exe)的 PID;找不到返回 0。
DWORD FindProcessInSession(const wchar_t* exeName, DWORD sessionId);

// 返回当前实际可交互的会话。优先活动控制台，会话桌面型系统中回退到任一 WTSActive
// 会话，因而兼容 RDP 登录；没有可用会话时返回 kInvalidSessionId。
DWORD FindActiveInteractiveSessionId();

// 返回指定交互会话的登录用户 SID（SDDL 字符串形式）。Service 用它把首次密码
// 通道仅授权给实际接收 Tray 的用户；调用方必须拥有 WTSQueryUserToken 所需权限。
bool GetSessionUserSidString(DWORD sessionId, std::wstring& sidString);

// 复制 srcPid 的 token,以 TokenPrimary + SecurityIdentification 复制成主令牌,
// 然后用 CreateProcessAsUserW 启动 commandLine,桌面设为 desktop。
// 用于让 LocalSystem 服务把子进程送入交互式会话(含 Winlogon 桌面)。
// 成功时由 processOut 接管进程句柄;调用方负责 CloseHandle。
bool LaunchChildWithProcessToken(DWORD srcPid, const std::wstring& commandLine,
                                 const std::wstring& desktop, HANDLE* processOut = nullptr);

// 在当前活动交互会话里,以 winlogon.exe 的 token 启动本 exe 的 --agent 模式,
// 桌面 winsta0\default。agent 进而在 agent 模式内 OpenInputDesktop 切到当前桌面。
bool LaunchAgentInConsoleSession(const std::wstring& exePath, HANDLE* processOut = nullptr);

// 在指定交互会话里,以 explorer.exe 的 token 启动 --tray 模式到用户桌面。
// 传入 sessionId 能确保首次密码通道的读权限与实际 Tray 会话一致。
// initialPasswordChannel 是 Service 创建的随机只读共享内存名称，不包含明文密码。
bool LaunchTrayInSession(const std::wstring& exePath, DWORD sessionId,
                         const std::wstring& initialPasswordChannel,
                         HANDLE* processOut = nullptr);

}  // namespace remote_assist
