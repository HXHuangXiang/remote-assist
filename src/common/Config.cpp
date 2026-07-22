#include "common/Config.h"

#include "common/Log.h"

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
    const std::wstring tempPath = path + L".tmp";
    {
        std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f << j.dump(2);
        f.flush();
        if (!f) {
            return false;
        }
    }
    return MoveFileExW(tempPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
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
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe = buf;
    size_t pos = exe.find_last_of(L"\\/");
    std::wstring dir = (pos != std::wstring::npos) ? exe.substr(0, pos) : exe;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring LogDir() {
    std::wstring dir = ConfigDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigFilePath() {
    return ConfigDir() + L"\\config.json";
}

Config LoadOrCreateConfig() {
    Config cfg;
    const std::wstring path = ConfigFilePath();
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
        log::Warn("config invalid, regenerating defaults");
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

        // 把明文密码写到一个 only-once 文件,供 tray 进程读取后删除;
        // 首次生成的密码不进入 service 的命令行或长期存储。
        {
            std::wstring pwPath = ConfigDir() + L"\\.initial-password";
            std::ofstream pf(pwPath, std::ios::binary | std::ios::trunc);
            if (pf) {
                pf.write(cfg.initialPassword.data(),
                         static_cast<std::streamsize>(cfg.initialPassword.size()));
            }
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
    if (!IsConfigValid(cfg) || token.empty()) {
        return false;
    }
    const std::string calculated = cfg.passwordIterations > 0
        ? Pbkdf2Sha256Hex(cfg.salt, token, cfg.passwordIterations)
        : Sha256Hex(cfg.salt, token);
    return ConstantTimeEquals(calculated, cfg.passwordHash);
}

bool SaveConfig(const Config& cfg) {
    if (!IsConfigValid(cfg) || !WriteConfigAtomically(cfg)) {
        log::Error("config save failed");
        return false;
    }
    return true;
}

bool SetPassword(Config& cfg, const std::string& password) {
    if (!IsHex(cfg.salt, 32)) cfg.salt = GenRandomHex(16);
    cfg.passwordIterations = kPasswordIterations;
    cfg.passwordHash = Pbkdf2Sha256Hex(cfg.salt, password, cfg.passwordIterations);
    return SaveConfig(cfg);
}

}  // namespace remote_assist
