#include "common/Config.h"

#include "common/Log.h"
#include "common/Path.h"

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace remote_assist {

namespace {

constexpr int kMinPort = 1;
constexpr int kMaxPort = 65535;
constexpr int kMinFps = 1;
constexpr int kMaxFps = 60;
constexpr int kMinBitrate = 100'000;
constexpr int kMaxBitrate = 50'000'000;
constexpr int kPasswordIterations = 210'000;
constexpr int kMaxPasswordIterations = 1'000'000;
constexpr DWORD kConfigLockTimeoutMs = 5000;
constexpr DWORD kConfigLockRetryMs = 50;

std::atomic<uint64_t> g_tempFileSequence{0};

// service 在 Session 0，配置窗口/托盘在交互会话，单靠进程内 mutex 无法避免两边
// 同时创建首份配置或覆盖对方刚保存的端口/密码。以 exe 同目录的锁文件建立跨会话
// 排他锁；锁句柄关闭时系统会自动释放，即使调用进程崩溃也不会永久阻塞后续修复。
class ConfigFileLock {
public:
    ConfigFileLock() = default;
    ~ConfigFileLock() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;

    bool Acquire() {
        const std::wstring dir = ConfigDir();
        if (dir.empty()) {
            return false;
        }
        const std::wstring lockPath = dir + L"\\.config.lock";
        const ULONGLONG deadline = GetTickCount64() + kConfigLockTimeoutMs;
        for (;;) {
            handle_ = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return true;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) {
                return false;
            }
            if (GetTickCount64() >= deadline) {
                SetLastError(ERROR_TIMEOUT);
                return false;
            }
            Sleep(kConfigLockRetryMs);
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::wstring MakeUniqueTempPath(const std::wstring& targetPath) {
    const uint64_t sequence = g_tempFileSequence.fetch_add(1, std::memory_order_relaxed);
    return targetPath + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(sequence) + L".tmp";
}

bool WriteUtf8FileAndFlush(const std::wstring& path, const std::string& contents) {
    if (contents.size() > MAXDWORD) {
        SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool ok = true;
    size_t written = 0;
    while (written < contents.size()) {
        const DWORD remaining = static_cast<DWORD>(std::min<size_t>(
            contents.size() - written, MAXDWORD));
        DWORD chunkWritten = 0;
        if (!WriteFile(file, contents.data() + written, remaining, &chunkWritten, nullptr) ||
            chunkWritten == 0) {
            ok = false;
            break;
        }
        written += chunkWritten;
    }
    if (ok && !FlushFileBuffers(file)) {
        ok = false;
    }
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        SetLastError(error);
    }
    return ok;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool IsHex(const std::string& value, size_t expectedLength) {
    if (value.size() != expectedLength) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return HexValue(ch) >= 0;
    });
}

bool HexToBytes(const std::string& value, std::vector<uint8_t>& bytes) {
    if (!IsHex(value, 32)) {
        return false;
    }
    bytes.clear();
    bytes.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>((HexValue(value[i]) << 4) |
                                             HexValue(value[i + 1])));
    }
    return true;
}

std::string ToHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(kHex[(data[i] >> 4) & 0xF]);
        s.push_back(kHex[data[i] & 0xF]);
    }
    return s;
}

// 用 CNG BCrypt 计算 SHA-256(saltBytes || tokenUtf8),返回 hex 小写。
std::string Sha256Hex(const std::string& saltHex, const std::string& token) {
    std::vector<uint8_t> saltBytes;
    if (!HexToBytes(saltHex, saltBytes)) {
        return {};
    }
    std::vector<uint8_t> input = saltBytes;
    input.insert(input.end(), token.begin(), token.end());

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    uint8_t hash[32] = {};
    // BCryptHash 的参数数量在不同 SDK 版本有差异(8 或 9 参数),
    // 改用稳定的 CreateHash/HashData/FinishHash 三步 API(Vista 起签名固定)。
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS st = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (st == 0) {
        st = BCryptHashData(hHash, input.data(), static_cast<ULONG>(input.size()), 0);
        if (st == 0) {
            st = BCryptFinishHash(hHash, hash, sizeof(hash), 0);
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st != 0) {
        return {};
    }
    return ToHex(hash, sizeof(hash));
}

// 通过 PBKDF2-SHA256 拉伸密码，降低局域网口令被离线/在线猜测的速度。
std::string Pbkdf2Sha256Hex(const std::string& saltHex, const std::string& token,
                            int iterations) {
    std::vector<uint8_t> saltBytes;
    if (!HexToBytes(saltHex, saltBytes) || iterations <= 0) {
        return {};
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    uint8_t hash[32] = {};
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(token.data())),
        static_cast<ULONG>(token.size()),
        saltBytes.data(), static_cast<ULONG>(saltBytes.size()),
        static_cast<ULONGLONG>(iterations),
        hash, sizeof(hash), 0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return status == 0 ? ToHex(hash, sizeof(hash)) : std::string();
}

std::string GenRandomHex(size_t bytes) {
    std::vector<uint8_t> buf(bytes);
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    return ToHex(buf.data(), buf.size());
}

// 生成易输入的 16 位大写 hex 密码(8 字节随机)。
std::string GenReadablePassword() {
    std::string h = GenRandomHex(8);
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return h;
}

bool IsConfigValid(const Config& cfg) {
    return cfg.port >= kMinPort && cfg.port <= kMaxPort &&
           cfg.fps >= kMinFps && cfg.fps <= kMaxFps &&
           cfg.bitrate >= kMinBitrate && cfg.bitrate <= kMaxBitrate &&
           (cfg.passwordIterations == 0 ||
               (cfg.passwordIterations >= kPasswordIterations &&
                cfg.passwordIterations <= kMaxPasswordIterations)) &&
           IsHex(cfg.salt, 32) && IsHex(cfg.passwordHash, 64);
}

bool WriteConfigAtomically(const Config& cfg) {
    nlohmann::json j;
    j["port"] = cfg.port;
    j["password_hash"] = cfg.passwordHash;
    j["salt"] = cfg.salt;
    j["password_iterations"] = cfg.passwordIterations;
    j["bitrate"] = cfg.bitrate;
    j["fps"] = cfg.fps;

    const std::wstring path = ConfigFilePath();
    if (path.empty()) {
        return false;
    }
    const std::wstring tempPath = MakeUniqueTempPath(path);
    if (!WriteUtf8FileAndFlush(tempPath, j.dump(2))) {
        return false;
    }
    if (MoveFileExW(tempPath.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    const DWORD error = GetLastError();
    DeleteFileW(tempPath.c_str());
    SetLastError(error);
    return false;
}

std::wstring InitialPasswordHintPath() {
    const std::wstring dir = ConfigDir();
    return dir.empty() ? std::wstring() : dir + L"\\.initial-password";
}

// 首次密码提示只在当前启动链路中短暂存在。用替换写避免 TrayApp 恰好读取时看到
// 半写入内容；写入失败时调用方会删除旧提示，宁可不提示也不能展示失效密码。
bool WriteInitialPasswordHint(const std::string& password) {
    if (password.empty()) {
        return false;
    }
    const std::wstring path = InitialPasswordHintPath();
    if (path.empty()) {
        return false;
    }
    const std::wstring tempPath = MakeUniqueTempPath(path);
    if (!WriteUtf8FileAndFlush(tempPath, password)) {
        return false;
    }
    if (MoveFileExW(tempPath.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(tempPath.c_str());
    return false;
}

void RemoveInitialPasswordHint() {
    const std::wstring path = InitialPasswordHintPath();
    if (path.empty()) {
        return;
    }
    if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        log::Warn("initial password hint removal failed: " + std::to_string(GetLastError()));
    }
}

bool ConstantTimeEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return difference == 0;
}

}  // namespace

std::wstring ConfigDir() {
    const std::wstring dir = ModuleDirectory();
    if (!dir.empty()) {
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

std::wstring LogDir() {
    const std::wstring configDir = ConfigDir();
    if (configDir.empty()) {
        return {};
    }
    std::wstring dir = configDir + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigFilePath() {
    const std::wstring dir = ConfigDir();
    return dir.empty() ? std::wstring() : dir + L"\\config.json";
}

Config LoadOrCreateConfig() {
    Config cfg;
    ConfigFileLock configLock;
    if (!configLock.Acquire()) {
        log::Error("config lock acquisition failed: " + std::to_string(GetLastError()));
        return cfg;
    }
    const std::wstring path = ConfigFilePath();
    if (path.empty()) {
        log::Error("config path resolution failed");
        return Config{};
    }
    std::ifstream f(path, std::ios::binary);
    bool loaded = false;
    const bool configFileFound = static_cast<bool>(f);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        try {
            auto j = nlohmann::json::parse(ss.str(), nullptr, false);
            if (j.is_object()) {
                cfg.port = j.value("port", cfg.port);
                cfg.passwordHash = j.value("password_hash", cfg.passwordHash);
                cfg.salt = j.value("salt", cfg.salt);
                cfg.passwordIterations = j.value("password_iterations", 0);
                cfg.bitrate = j.value("bitrate", cfg.bitrate);
                cfg.fps = j.value("fps", cfg.fps);
                loaded = IsConfigValid(cfg);
            }
        } catch (...) {
            loaded = false;
        }
    }

    if (!loaded && configFileFound) {
        // 已存在的配置一旦损坏，不能静默覆盖并随机生成新密码：用户会失去原有
        // 访问口令，且托盘可能展示与其预期不一致的新密码。保留原文件供排查，
        // 由配置窗口设置新密码后显式修复。
        RemoveInitialPasswordHint();
        log::Error("config invalid, preserving file for repair in setup dialog");
        return Config{};
    }
    if (!loaded) {
        cfg = Config{};
    }

    if (cfg.passwordHash.empty() || cfg.salt.empty()) {
        cfg.salt = GenRandomHex(16);
        cfg.initialPassword = GenReadablePassword();
        cfg.passwordIterations = kPasswordIterations;
        cfg.passwordHash = Pbkdf2Sha256Hex(cfg.salt, cfg.initialPassword,
                                           cfg.passwordIterations);

        if (!IsConfigValid(cfg)) {
            log::Error("config generation failed");
            return Config{};
        }

        if (!WriteConfigAtomically(cfg)) {
            log::Error("config write failed: " + std::to_string(GetLastError()));
            return Config{};
        }
        // 把明文密码写到一个 only-once 文件,供 tray 进程读取后删除；它只在
        // config 已成功落盘后创建，避免遗留不对应任何有效配置的旧提示。
        if (!WriteInitialPasswordHint(cfg.initialPassword)) {
            log::Warn("initial password hint write failed");
        }
        log::Info("config generated with new password (len=" +
                  std::to_string(cfg.initialPassword.size()) + ")");
    } else if (loaded) {
        log::Info("config loaded from disk");
    }
    return cfg;
}

bool VerifyPassword(const Config& cfg, const std::string& token) {
    if (!IsConfigValid(cfg) || token.empty()) {
        return false;
    }
    const std::string calculated = cfg.passwordIterations > 0
        ? Pbkdf2Sha256Hex(cfg.salt, token, cfg.passwordIterations)
        : Sha256Hex(cfg.salt, token);
    return ConstantTimeEquals(calculated, cfg.passwordHash);
}

bool SaveConfig(const Config& cfg) {
    ConfigFileLock configLock;
    if (!configLock.Acquire()) {
        log::Error("config lock acquisition failed: " + std::to_string(GetLastError()));
        return false;
    }
    if (!IsConfigValid(cfg) || !WriteConfigAtomically(cfg)) {
        log::Error("config save failed");
        return false;
    }
    return true;
}

bool SetPassword(Config& cfg, const std::string& password) {
    if (password.empty()) {
        log::Warn("password update rejected: empty password");
        return false;
    }
    ConfigFileLock configLock;
    if (!configLock.Acquire()) {
        log::Error("config lock acquisition failed: " + std::to_string(GetLastError()));
        return false;
    }
    if (!IsHex(cfg.salt, 32)) cfg.salt = GenRandomHex(16);
    cfg.passwordIterations = kPasswordIterations;
    cfg.passwordHash = Pbkdf2Sha256Hex(cfg.salt, password, cfg.passwordIterations);
    if (!IsConfigValid(cfg) || !WriteConfigAtomically(cfg)) {
        log::Error("config save failed");
        return false;
    }

    // 配置窗口在首次运行时已经持有随机初始密码。若用户改为自定义密码（例如
    // 123456）再安装服务，TrayApp 必须展示新密码而不是旧随机值。普通密码变更
    // 则删除任何残留提示，避免服务重启后泄露/展示已经失效的旧密码。
    const bool shouldRefreshHint = !cfg.initialPassword.empty();
    cfg.initialPassword.clear();
    if (shouldRefreshHint) {
        if (!WriteInitialPasswordHint(password)) {
            RemoveInitialPasswordHint();
            log::Warn("initial password hint refresh failed");
        }
    } else {
        RemoveInitialPasswordHint();
    }
    return true;
}

}  // namespace remote_assist
