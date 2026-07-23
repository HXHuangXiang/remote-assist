#include "service/ProcessLauncher.h"

#include "common/Log.h"
#include "common/Path.h"

#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <sddl.h>

#include <array>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace remote_assist {

DWORD FindProcessInSession(const wchar_t* exeName, DWORD sessionId) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD result = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                DWORD pidSession = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &pidSession) &&
                    pidSession == sessionId) {
                    result = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

DWORD FindActiveInteractiveSessionId() {
    const DWORD consoleSession = WTSGetActiveConsoleSessionId();
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD sessionCount = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount)) {
        return consoleSession;
    }

    DWORD fallbackSession = kInvalidSessionId;
    for (DWORD index = 0; index < sessionCount; ++index) {
        if (sessions[index].State != WTSActive) {
            continue;
        }
        if (sessions[index].SessionId == consoleSession) {
            WTSFreeMemory(sessions);
            return consoleSession;
        }
        if (fallbackSession == kInvalidSessionId) {
            fallbackSession = sessions[index].SessionId;
        }
    }
    WTSFreeMemory(sessions);
    return fallbackSession != kInvalidSessionId ? fallbackSession : consoleSession;
}

bool GetSessionUserSidString(DWORD sessionId, std::wstring& sidString) {
    sidString.clear();
    if (sessionId == kInvalidSessionId) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    HANDLE token = nullptr;
    if (!WTSQueryUserToken(sessionId, &token)) {
        log::Warn("WTSQueryUserToken failed for session=" + std::to_string(sessionId) +
                  ": " + std::to_string(GetLastError()));
        return false;
    }

    DWORD tokenUserBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenUserBytes);
    const DWORD sizeError = GetLastError();
    if (tokenUserBytes == 0 || sizeError != ERROR_INSUFFICIENT_BUFFER) {
        log::Warn("TokenUser size query failed for session=" + std::to_string(sessionId) +
                  ": " + std::to_string(sizeError));
        CloseHandle(token);
        SetLastError(sizeError);
        return false;
    }
    std::vector<BYTE> tokenUserBuffer(tokenUserBytes);
    if (!GetTokenInformation(token, TokenUser, tokenUserBuffer.data(), tokenUserBytes,
                             &tokenUserBytes)) {
        const DWORD error = GetLastError();
        log::Warn("TokenUser query failed for session=" + std::to_string(sessionId) +
                  ": " + std::to_string(error));
        CloseHandle(token);
        SetLastError(error);
        return false;
    }

    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenUserBuffer.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText)) {
        const DWORD error = GetLastError();
        log::Warn("ConvertSidToStringSid failed for session=" + std::to_string(sessionId) +
                  ": " + std::to_string(error));
        CloseHandle(token);
        SetLastError(error);
        return false;
    }
    sidString = sidText;
    LocalFree(sidText);
    CloseHandle(token);
    return !sidString.empty();
}

// Agent 需要使用真实 winlogon 的 LocalSystem token 才能在锁屏 input desktop 上稳定
// 采集和注入。仅按进程名匹配会被同会话内的同名普通进程误导，因此额外校验镜像路径
// 与 TokenUser SID；校验失败时宁可等待下一轮服务监控，也不启动半权限 Agent。
bool IsExpectedLocalSystemProcess(DWORD pid, const wchar_t* expectedExeName) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    std::array<wchar_t, 32768> imagePath{};
    DWORD imagePathLength = static_cast<DWORD>(imagePath.size());
    bool validImage = QueryFullProcessImageNameW(process, 0, imagePath.data(), &imagePathLength) != FALSE;
    wchar_t systemDirectory[MAX_PATH] = {};
    if (validImage && GetSystemDirectoryW(systemDirectory, MAX_PATH) != 0) {
        const std::wstring expectedPath = std::wstring(systemDirectory) + L"\\" + expectedExeName;
        validImage = _wcsicmp(imagePath.data(), expectedPath.c_str()) == 0;
    } else {
        validImage = false;
    }

    bool validOwner = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        DWORD tokenUserSize = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &tokenUserSize);
        std::vector<BYTE> tokenUserBuffer(tokenUserSize);
        if (tokenUserSize != 0 && GetTokenInformation(token, TokenUser, tokenUserBuffer.data(),
                                                       tokenUserSize, &tokenUserSize)) {
            const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenUserBuffer.data());
            DWORD systemSidSize = SECURITY_MAX_SID_SIZE;
            std::array<BYTE, SECURITY_MAX_SID_SIZE> systemSidBuffer{};
            if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSidBuffer.data(),
                                   &systemSidSize)) {
                validOwner = EqualSid(tokenUser->User.Sid, systemSidBuffer.data()) != FALSE;
            }
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return validImage && validOwner;
}

bool LaunchChildWithProcessToken(DWORD srcPid, const std::wstring& commandLine,
                                 const std::wstring& desktop, HANDLE* processOut) {
    if (processOut) {
        *processOut = nullptr;
    }
    const HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, srcPid);
    if (!hProc) {
        log::Error("OpenProcess(pid=" + std::to_string(srcPid) +
                   ") failed: " + std::to_string(GetLastError()));
        return false;
    }
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(hProc,
            TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY |
            TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, &hToken)) {
        log::Error("OpenProcessToken failed: " + std::to_string(GetLastError()));
        CloseHandle(hProc);
        return false;
    }
    HANDLE hPrimary = nullptr;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr,
            SecurityIdentification, TokenPrimary, &hPrimary)) {
        log::Error("DuplicateTokenEx failed: " + std::to_string(GetLastError()));
        CloseHandle(hToken);
        CloseHandle(hProc);
        return false;
    }

    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');

    void* env = nullptr;
    CreateEnvironmentBlock(&env, hPrimary, FALSE);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessAsUserW(
        hPrimary, nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, nullptr, &si, &pi);

    if (env) {
        DestroyEnvironmentBlock(env);
    }
    if (ok) {
        CloseHandle(pi.hThread);
        if (processOut) {
            *processOut = pi.hProcess;
        } else {
            CloseHandle(pi.hProcess);
        }
        // commandLine 可能携带一次性共享内存名称等敏感的短期凭据；日志只保留
        // 成功事实，角色和会话由上层调用点记录。
        log::Info("child process launched");
    } else {
        log::Error("CreateProcessAsUserW failed: " + std::to_string(GetLastError()));
    }

    CloseHandle(hPrimary);
    CloseHandle(hToken);
    CloseHandle(hProc);
    return ok == TRUE;
}

bool LaunchAgentInConsoleSession(const std::wstring& exePath, HANDLE* processOut) {
    const DWORD sid = FindActiveInteractiveSessionId();
    if (sid == kInvalidSessionId) {
        log::Warn("no active interactive session, skip agent launch");
        return false;
    }
    const DWORD winlogonPid = FindProcessInSession(L"winlogon.exe", sid);
    if (!winlogonPid) {
        log::Warn("winlogon.exe not found in active interactive session");
        return false;
    }
    if (!IsExpectedLocalSystemProcess(winlogonPid, L"winlogon.exe")) {
        log::Warn("winlogon.exe validation failed; skip agent launch");
        return false;
    }
    const std::wstring cmd = L"\"" + exePath + L"\" --agent --service-managed";
    const bool launched = LaunchChildWithProcessToken(winlogonPid, cmd, L"winsta0\\default",
                                                       processOut);
    if (launched) {
        log::Info("agent launched in session=" + std::to_string(sid));
    }
    return launched;
}

bool LaunchTrayInSession(const std::wstring& exePath, DWORD sessionId,
                         const std::wstring& initialPasswordChannel,
                         HANDLE* processOut) {
    if (sessionId == kInvalidSessionId) {
        return false;
    }
    const DWORD explorerPid = FindProcessInSession(L"explorer.exe", sessionId);
    if (!explorerPid) {
        log::Warn("explorer.exe not found in active interactive session, skip tray");
        return false;
    }
    std::wstring cmd = L"\"" + exePath + L"\" --tray";
    if (!initialPasswordChannel.empty()) {
        cmd += L" --initial-password-channel \"" + initialPasswordChannel + L"\"";
    }
    const bool launched = LaunchChildWithProcessToken(explorerPid, cmd, L"winsta0\\default",
                                                       processOut);
    if (launched) {
        log::Info("tray launched in session=" + std::to_string(sessionId));
    }
    return launched;
}

}  // namespace remote_assist
