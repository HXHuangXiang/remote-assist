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
#include <utility>
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
constexpr size_t kMaxPasswordBytes = 3072;
constexpr int kPasswordIterations = 210'000;
// 已发布版本曾使用低于当前推荐值的 PBKDF2 参数。读取时必须接受所有正整数
// 迭代次数，才能让用户保留原密码；认证成功后会升级到 kPasswordIterations。
constexpr int kMinPasswordIterations = 1;
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
    // PBKDF2 的 PRF 是 HMAC-SHA256，而不是裸 SHA-256。未指定 HMAC 标志时，
    // BCryptDeriveKeyPBKDF2 会在部分 Windows CNG 实现上返回无效句柄，导致首次
    // 配置和改密无法生成哈希。
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
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
           IsQualityCapValid(cfg.qualityCap) &&
           (cfg.passwordIterations == 0 ||
               (cfg.passwordIterations >= kMinPasswordIterations &&
                cfg.passwordIterations <= kMaxPasswordIterations)) &&
           IsHex(cfg.salt, 32) && IsHex(cfg.passwordHash, 64);
}

// 读取已存在配置并完成统一校验。调用方决定“文件不存在”和“文件存在但无效”分别
// 应如何处理：服务启动要保留损坏文件供用户修复，Tray 的只读查询则直接失败。
bool LoadExistingConfigFile(const std::wstring& path, Config& cfg) {
    cfg = Config{};
    if (path.empty()) {
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        const auto j = nlohmann::json::parse(ss.str(), nullptr, false);
        if (!j.is_object()) {
            return false;
        }
        cfg.port = j.value("port", cfg.port);
        cfg.passwordHash = j.value("password_hash", cfg.passwordHash);
        cfg.salt = j.value("salt", cfg.salt);
        cfg.passwordIterations = j.value("password_iterations", 0);
        cfg.bitrate = j.value("bitrate", cfg.bitrate);
        cfg.fps = j.value("fps", cfg.fps);
        cfg.qualityCap = j.value("quality_cap", cfg.qualityCap);
        return IsConfigValid(cfg);
    } catch (...) {
        return false;
    }
}

bool SamePasswordStorage(const Config& left, const Config& right) {
    return left.passwordHash == right.passwordHash && left.salt == right.salt &&
           left.passwordIterations == right.passwordIterations;
}

bool WriteConfigAtomically(const Config& cfg) {
    nlohmann::json j;
    j["port"] = cfg.port;
    j["password_hash"] = cfg.passwordHash;
    j["salt"] = cfg.salt;
    j["password_iterations"] = cfg.passwordIterations;
    j["bitrate"] = cfg.bitrate;
    j["fps"] = cfg.fps;
    j["quality_cap"] = cfg.qualityCap;

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

std::wstring LegacyInitialPasswordHintPath() {
    const std::wstring dir = ConfigDir();
    return dir.empty() ? std::wstring() : dir + L"\\.initial-password";
}

// 旧版本把首个密码明文写到 exe 目录。新版本用短生命周期内存通道交付给 Tray；
// 配置加载时尽力清理历史遗留，避免升级后继续在磁盘保留明文。
void RemoveLegacyInitialPasswordHint() {
    const std::wstring path = LegacyInitialPasswordHintPath();
    if (path.empty()) {
        return;
    }
    if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        log::Warn("legacy initial password hint removal failed: " +
                  std::to_string(GetLastError()));
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
    // 可执行文件所在目录必然已经存在。此前在这里调用 CreateDirectoryW 会让
    // LoadConfigReadOnly 也产生写入尝试，普通用户读取受保护安装目录时会留下
    // 无意义的拒绝访问错误。
    return ModuleDirectory();
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

bool LoadConfigReadOnly(Config& cfg) {
    return LoadExistingConfigFile(ConfigFilePath(), cfg);
}

Config LoadOrCreateConfig() {
    Config cfg;
    ConfigFileLock configLock;
    if (!configLock.Acquire()) {
        log::Error("config lock acquisition failed: " + std::to_string(GetLastError()));
        return cfg;
    }
    RemoveLegacyInitialPasswordHint();
    const std::wstring path = ConfigFilePath();
    if (path.empty()) {
        log::Error("config path resolution failed");
        return Config{};
    }
    // 目录存在但无读取权限、被替换为目录或内容损坏时，都应视为“已有但不可用”，
    // 不能误当作首次安装而随机生成新密码。实际 JSON 解析复用只读加载路径，保证
    // Tray、服务和迁移逻辑对字段缺失/范围校验有完全一致的语义。
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const bool configFileFound = GetFileAttributesExW(
        path.c_str(), GetFileExInfoStandard, &attributes) != FALSE &&
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    const bool loaded = LoadExistingConfigFile(path, cfg);

    if (!loaded && configFileFound) {
        // 已存在的配置一旦损坏，不能静默覆盖并随机生成新密码：用户会失去原有
        // 访问口令，且托盘可能展示与其预期不一致的新密码。保留原文件供排查，
        // 由配置窗口设置新密码后显式修复。
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
        log::Info("config generated with new password (len=" +
                  std::to_string(cfg.initialPassword.size()) + ")");
    } else if (loaded) {
        log::Info("config loaded from disk");
    }
    return cfg;
}

bool VerifyPassword(const Config& cfg, const std::string& token) {
    if (!IsConfigValid(cfg) || token.empty() || token.size() > kMaxPasswordBytes) {
        return false;
    }
    const std::string calculated = cfg.passwordIterations > 0
        ? Pbkdf2Sha256Hex(cfg.salt, token, cfg.passwordIterations)
        : Sha256Hex(cfg.salt, token);
    return ConstantTimeEquals(calculated, cfg.passwordHash);
}

bool VerifyAndUpgradePassword(Config& cfg, const std::string& token) {
    if (!VerifyPassword(cfg, token)) {
        return false;
    }
    if (cfg.passwordIterations >= kPasswordIterations) {
        return true;
    }

    // 历史 SHA-256 和早期较低迭代次数的 PBKDF2 都只在口令已被证明正确后才迁移，
    // 避免攻击者用错误请求触发高成本 PBKDF2 或改写配置。即使迁移落盘失败，本次
    // 认证仍然已经有效。
    ConfigFileLock configLock;
    if (!configLock.Acquire()) {
        log::Warn("password storage upgrade lock acquisition failed: " +
                  std::to_string(GetLastError()));
        return true;
    }

    // 配置窗口保存后会异步重启服务。若旧 Agent 恰好在重启前完成一次历史口令
    // 认证，不能用内存中的旧配置覆盖用户刚设置的新密码；在持有跨进程锁后再次
    // 确认磁盘中的 password storage 与本实例仍一致，才允许执行迁移。
    Config persisted;
    if (!LoadExistingConfigFile(ConfigFilePath(), persisted) ||
        !SamePasswordStorage(cfg, persisted)) {
        log::Info("password storage upgrade skipped because config changed");
        return true;
    }

    Config upgraded = cfg;
    upgraded.salt = GenRandomHex(16);
    upgraded.passwordIterations = kPasswordIterations;
    upgraded.passwordHash = Pbkdf2Sha256Hex(upgraded.salt, token,
                                            upgraded.passwordIterations);
    if (!IsConfigValid(upgraded) || !WriteConfigAtomically(upgraded)) {
        log::Warn("password storage upgrade to PBKDF2-SHA256 failed");
        return true;
    }
    cfg = std::move(upgraded);
    log::Info("password storage upgraded to PBKDF2-SHA256");
    return true;
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
    if (password.empty() || password.size() > kMaxPasswordBytes) {
        log::Warn("password update rejected: invalid length");
        return false;
    }
    ConfigFileLock configLock;
    if (!configLock.Acquire()) {
        log::Error("config lock acquisition failed: " + std::to_string(GetLastError()));
        return false;
    }
    // 每次改密都生成新 salt；复用 salt 虽不会破坏单个 PBKDF2 哈希的正确性，却会
    // 让同一密码在多次改回时生成相同哈希，也不利于后续安全参数演进。
    Config updated = cfg;
    updated.salt = GenRandomHex(16);
    updated.passwordIterations = kPasswordIterations;
    updated.passwordHash = Pbkdf2Sha256Hex(updated.salt, password,
                                           updated.passwordIterations);
    if (!IsConfigValid(updated) || !WriteConfigAtomically(updated)) {
        log::Error("config save failed");
        return false;
    }

    // 仅当前进程刚生成配置时保留首个密码，以便安装完成后由配置窗口明确展示。
    // 后续常规改密不会把明文重新保存到 Config，也不会落入磁盘临时文件。
    if (!updated.initialPassword.empty()) {
        updated.initialPassword = password;
    }
    cfg = std::move(updated);
    return true;
}

}  // namespace remote_assist
