#pragma once

#include <windows.h>
#include <string>

namespace remote_assist {

// 双击 exe 时弹出的配置窗口:设密码、安装/启动/停止服务。
int RunSetupDialog(HINSTANCE hInst);

}  // namespace remote_assist
