#pragma once

#include <windows.h>

#include <string>

namespace remote_assist {

// 在当前线程桌面(已 SetThreadDesktop)上注入键鼠事件。
// 键盘走 scancode 路径(KEYEVENTF_SCANCODE),对锁屏/Winlogon 桌面更兼容。
class Input {
public:
    // Agent 启动后开启输入；停止时先关闭，阻止断连清理期间的新输入再次落到桌面。
    static void Enable();
    static void Disable();
    static bool SendKey(USHORT scancode, bool down, bool extended = false);
    static bool SendMouseAbs(double x, double y);
    static bool SendMouseButton(const std::string& button, bool down);
    static bool SendWheel(int delta);

    // 控制端断开、页面失焦或服务停止时释放仍处于按下状态的按键和鼠标键。
    // 调用方必须已绑定目标 desktop，避免远端因丢失 keyup/mouseup 而长期卡住状态。
    // 失败的状态会保留，以便在桌面切换恢复后重试；返回 true 表示所有输入均释放。
    static bool ReleaseAll();
};

}  // namespace remote_assist
