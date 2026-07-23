#pragma once

#include <string>

namespace remote_assist {

// 用户配置的输出质量上限。自动模式沿用当前自适应策略的最高 1080p 档；指定档位
// 会限制自适应恢复时的最高分辨率，但网络、编码或采集过载时仍可继续向下调档。
enum class QualityCap : int {
    kAutomatic = 0,
    k1080p = 1080,
    k720p = 720,
    k540p = 540,
};

struct Config {
    int port = 7980;
    std::string passwordHash;
    std::string salt;
    // 0 表示兼容历史 SHA-256；正数表示 PBKDF2-SHA256 的迭代次数。
    int passwordIterations = 0;
    int bitrate = 4'000'000;
    int fps = 30;
    int qualityCap = static_cast<int>(QualityCap::kAutomatic);
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
// 配置窗口与配置校验共用，避免 UI 选择项和磁盘配置允许值发生漂移。
constexpr bool IsQualityCapValid(int qualityCap) {
    switch (static_cast<QualityCap>(qualityCap)) {
    case QualityCap::kAutomatic:
    case QualityCap::k1080p:
    case QualityCap::k720p:
    case QualityCap::k540p:
        return true;
    }
    return false;
}

}  // namespace remote_assist
