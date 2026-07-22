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
std::mutex g_mu;
constexpr ULONGLONG kMaxLogBytes = 5ULL * 1024 * 1024;
constexpr int kLogBackupCount = 3;

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

void RotateLogIfNeeded(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return;
    }
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    if (size.QuadPart < kMaxLogBytes) {
        return;
    }

    for (int index = kLogBackupCount - 1; index >= 1; --index) {
        const std::wstring source = path + L"." + std::to_wstring(index);
        const std::wstring target = path + L"." + std::to_wstring(index + 1);
        MoveFileExW(source.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    MoveFileExW(path.c_str(), (path + L".1").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}  // namespace

void Init(const std::wstring& dir) {
    g_dir = dir;
    if (!g_dir.empty()) {
        CreateDirectoryW(g_dir.c_str(), nullptr);
    }
}

void Write(const char* level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string line = "[" + Timestamp() + "] [" + level + "] " + msg + "\n";
    OutputDebugStringA(line.c_str());
    if (!g_dir.empty()) {
        const std::wstring path = g_dir + L"\\remote-assist.log";
        RotateLogIfNeeded(path);
        std::ofstream of(path, std::ios::app);
        if (of) {
            of << line;
        }
    }
}

void Info(const std::string& m) { Write("INFO", m); }
void Warn(const std::string& m) { Write("WARN", m); }
void Error(const std::string& m) { Write("ERROR", m); }

}  // namespace remote_assist::log
