#include "common/Log.h"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace remote_assist::log {

namespace {

std::wstring g_dir;
std::wstring g_fileName = L"remote-assist.log";
std::wstring g_pipeName;
std::mutex g_mu;
constexpr ULONGLONG kMaxLogBytes = 5ULL * 1024 * 1024;
constexpr int kLogBackupCount = 3;
constexpr size_t kMaxPipeLogPayloadBytes = 8 * 1024;
bool g_rotationFallbackReported = false;

std::string Timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
       << std::setfill('0') << std::setw(3) << ms.count();
    return os.str();
}

bool RotateLogIfNeeded(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        // 文件首次创建或暂时不可读取时无需在这里处理；后续 append 会给出最终结果。
        return true;
    }
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    if (size.QuadPart < kMaxLogBytes) {
        return true;
    }

    for (int index = kLogBackupCount - 1; index >= 1; --index) {
        const std::wstring source = path + L"." + std::to_wstring(index);
        const std::wstring target = path + L"." + std::to_wstring(index + 1);
        MoveFileExW(source.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    return MoveFileExW(path.c_str(), (path + L".1").c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// 备份文件被安全软件或日志查看器锁定时，主文件的原子改名也可能失败。此时若仍能
// 独占写入主文件，截断比无限追加更可控；若连截断都失败，则保留原文件并让本次写入
// 尽力执行，避免因为轮转问题丢失最近一条故障信息。
bool TruncateLog(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER zero{};
    const bool ok = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE &&
        SetEndOfFile(file) != FALSE && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
}

std::string MakeLine(const char* level, const std::string& msg) {
    return "[" + Timestamp() + "] [" + (level ? level : "INFO") + "] " + msg + "\n";
}

void AppendLineLocked(const std::wstring& fileName, const std::string& line) {
    if (g_dir.empty() || fileName.empty()) {
        return;
    }
    const std::wstring path = g_dir + L"\\" + fileName;
    if (!RotateLogIfNeeded(path)) {
        if (TruncateLog(path)) {
            g_rotationFallbackReported = false;
        } else if (!g_rotationFallbackReported) {
            OutputDebugStringW(L"remote-assist: log rotation and truncation both failed\n");
            g_rotationFallbackReported = true;
        }
    } else {
        g_rotationFallbackReported = false;
    }
    std::ofstream of(path, std::ios::app);
    if (of) {
        of << line;
    }
}

void SendPipeLogLocked(const char* level, const std::string& msg) {
    if (g_pipeName.empty()) {
        return;
    }
    std::string payload = std::string(level ? level : "INFO") + "\t" + msg;
    if (payload.empty() || payload.size() > kMaxPipeLogPayloadBytes) {
        return;
    }
    // Tray 的日志频率很低；每行独立连接可以避免服务重启或会话切换时遗留的
    // 长连接。CreateFile 不等待 busy pipe，失败时仅保留调试输出，不能阻塞 UI。
    const HANDLE pipe = CreateFileW(g_pipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    CloseHandle(pipe);
}

}  // namespace

void Init(const std::wstring& dir, const wchar_t* fileName) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dir = dir;
    g_fileName = (fileName && *fileName) ? fileName : L"remote-assist.log";
    g_pipeName.clear();
    g_rotationFallbackReported = false;
    if (!g_dir.empty()) {
        CreateDirectoryW(g_dir.c_str(), nullptr);
    }
}

void InitPipeSink(const std::wstring& pipeName) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dir.clear();
    g_fileName = L"tray.log";
    g_pipeName = pipeName;
    g_rotationFallbackReported = false;
}

void Write(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string line = MakeLine(level, msg);
    OutputDebugStringA(line.c_str());
    if (!g_pipeName.empty()) {
        SendPipeLogLocked(level, msg);
        return;
    }
    AppendLineLocked(g_fileName, line);
}

void WriteToFile(const wchar_t* fileName, const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string line = MakeLine(level, msg);
    OutputDebugStringA(line.c_str());
    AppendLineLocked((fileName && *fileName) ? fileName : g_fileName, line);
}

void Info(const std::string& m) { Write("INFO", m); }
void Warn(const std::string& m) { Write("WARN", m); }
void Error(const std::string& m) { Write("ERROR", m); }

}  // namespace remote_assist::log
