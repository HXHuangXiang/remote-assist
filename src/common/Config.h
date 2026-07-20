#pragma once

#include <string>

namespace remote_assist {

// 运行期配置。由 LoadOrCreateConfig 从 %ProgramData%\RemoteAssist\config.json 加载,
// 不存在时生成随机密码与 salt 并写回。明文密码仅在首次生成时出现在 initialPassword,
// 供托盘 UI 展示一次;之后只保存 SHA-256(salt+password) 的 hex。
struct Config {
    int port = 7980;
    std::string passwordHash;   // SHA-256(salt||password) 的 hex
    std::string salt;           // 16 字节随机 salt 的 hex
    int bitrate = 4'000'000;    // H.264 目标码率(bps)
    int fps = 30;
    std::string initialPassword;  // 仅首次生成时非空,托盘读后应清空
};

// 配置目录 %ProgramData%\RemoteAssist(不存在则创建)。
std::wstring ConfigDir();

// 日志目录 %ProgramData%\RemoteAssist\logs。
std::wstring LogDir();

// 配置文件完整路径。
std::wstring ConfigFilePath();

// 加载配置;不存在则生成并写回。返回的 Config 可能含 initialPassword。
Config LoadOrCreateConfig();

// 校验明文 token 是否与配置中的 passwordHash 匹配。
bool VerifyPassword(const Config& cfg, const std::string& token);

}  // namespace remote_assist

