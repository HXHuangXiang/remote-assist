#pragma once

#include <string>

namespace remote_assist {

struct Config {
    int port = 7980;
    std::string passwordHash;
    std::string salt;
    // 0 表示兼容历史 SHA-256；正数表示 PBKDF2-SHA256 的迭代次数。
    int passwordIterations = 0;
    int bitrate = 4'000'000;
    int fps = 30;
    std::string initialPassword;
};

// 配置目录(exe 同目录)。
std::wstring ConfigDir();
std::wstring LogDir();
std::wstring ConfigFilePath();

Config LoadOrCreateConfig();
// 原子写入配置，失败时返回 false。
bool SaveConfig(const Config& cfg);
// 更新密码哈希并保存配置，失败时返回 false。
bool SetPassword(Config& cfg, const std::string& password);
bool VerifyPassword(const Config& cfg, const std::string& token);

}  // namespace remote_assist
