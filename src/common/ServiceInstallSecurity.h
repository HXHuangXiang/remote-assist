#pragma once

#include <string>

namespace remote_assist {

// 以 LocalSystem 安装服务前的目录安全检查结果。服务会从 exe 同级读取网页、配置和
// 日志，因此不能静默复制到其他目录；但当前目录若允许普通帐户改写，则绝不能作为
// 高权限服务的映像路径。
struct ServiceInstallPathValidation {
    bool secure = false;
    std::wstring reason;
};

// 检查 exe、同级 web/ 资源树、已有配置/日志/首次密码提示及到卷根的目录链：拒绝
// 重解析点、网络/可移动卷、空 DACL、非管理员所有者和可由非管理员身份改写的
// 访问控制项。首次运行尚不存在的运行产物会保留给正常初始化流程创建。
ServiceInstallPathValidation ValidateServiceInstallPath(const std::wstring& exePath);

}  // namespace remote_assist
