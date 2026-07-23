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

// 配置目录(exe 同目录)。该查询不创建目录，保证只读调用不产生文件系统副作用。
std::wstring ConfigDir();
std::wstring LogDir();
std::wstring ConfigFilePath();

Config LoadOrCreateConfig();
// 只读取并校验已有配置，不创建锁文件、不生成默认密码。供普通用户会话中的 Tray
// 查询端口等公开运行信息，避免安全安装目录下缺少写权限导致读取失败。
bool LoadConfigReadOnly(Config& cfg);
// 原子写入配置，失败时返回 false。
bool SaveConfig(const Config& cfg);
// 更新密码哈希并保存配置，失败时返回 false。
bool SetPassword(Config& cfg, const std::string& password);
bool VerifyPassword(const Config& cfg, const std::string& token);
// 校验密码；若命中历史 SHA-256 格式或较低迭代次数的 PBKDF2，则在首次成功认证后
// 升级为带新 salt 的当前 PBKDF2-SHA256 参数。升级写入失败不影响本次已成功的认证，
// 后续认证会再次尝试迁移。
bool VerifyAndUpgradePassword(Config& cfg, const std::string& token);
}  // namespace remote_assist
