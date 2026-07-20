#include "common/Config.h"

#include "common/Log.h"

#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <windows.h>

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
    saltBytes.reserve(saltHex.size() / 2);
    for (size_t i = 0; i + 1 < saltHex.size(); i += 2) {
        const auto v = static_cast<uint8_t>(
            std::stoi(saltHex.substr(i, 2), nullptr, 16));
        saltBytes.push_back(v);
    }
    std::vector<uint8_t> input = saltBytes;
    input.insert(input.end(), token.begin(), token.end());

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    uint8_t hash[32] = {};
    ULONG outLen = 0;
    const NTSTATUS st = BCryptHash(hAlg, nullptr, 0, input.data(),
                                   static_cast<ULONG>(input.size()),
                                   hash, sizeof(hash), &outLen);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st != 0) {
        return {};
    }
    return ToHex(hash, sizeof(hash));
}

std::string GenRandomHex(size_t bytes) {
    std::vector<uint8_t> buf(bytes);
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RNG_ALGORITHM, nullptr, 0) == 0) {
        BCryptGenRandom(hAlg, buf.data(), static_cast<ULONG>(buf.size()), 0);
        BCryptCloseAlgorithmProvider(hAlg, 0);
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

}  // namespace

std::wstring ConfigDir() {
    wchar_t* programData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) &&
        programData) {
        dir = programData;
        dir += L"\\RemoteAssist";
        CoTaskMemFree(programData);
    }
    if (dir.empty()) {
        dir = L"C:\\ProgramData\\RemoteAssist";
    }
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
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        try {
            auto j = nlohmann::json::parse(ss.str(), nullptr, false);
            if (j.is_object()) {
                cfg.port = j.value("port", cfg.port);
                cfg.passwordHash = j.value("password_hash", cfg.passwordHash);
                cfg.salt = j.value("salt", cfg.salt);
                cfg.bitrate = j.value("bitrate", cfg.bitrate);
                cfg.fps = j.value("fps", cfg.fps);
                loaded = true;
            }
        } catch (...) {
            loaded = false;
        }
    }

    if (cfg.passwordHash.empty() || cfg.salt.empty()) {
        cfg.salt = GenRandomHex(16);
        cfg.initialPassword = GenReadablePassword();
        cfg.passwordHash = Sha256Hex(cfg.salt, cfg.initialPassword);

        nlohmann::json j;
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
        j["port"] = cfg.port;
        j["password_hash"] = cfg.passwordHash;
        j["salt"] = cfg.salt;
        j["bitrate"] = cfg.bitrate;
        j["fps"] = cfg.fps;
        std::ofstream of(path, std::ios::binary | std::ios::trunc);
        if (of) {
            of << j.dump(2);
        }
        log::Info("config generated with new password (len=" +
                  std::to_string(cfg.initialPassword.size()) + ")");
    } else if (loaded) {
        log::Info("config loaded from disk");
    }
    return cfg;
}

bool VerifyPassword(const Config& cfg, const std::string& token) {
    if (cfg.passwordHash.empty() || token.empty()) {
        return false;
    }
    return Sha256Hex(cfg.salt, token) == cfg.passwordHash;
}

}  // namespace remote_assist
