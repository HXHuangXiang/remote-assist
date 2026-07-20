#pragma once

namespace remote_assist {

// Windows 服务主入口。以 LocalSystem 运行,由 SCM 调度。
// 负责加载配置、启动 agent 与 tray,并在停止时优雅退出。
int RunAsService();

}  // namespace remote_assist

