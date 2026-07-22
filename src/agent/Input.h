#pragma once

#include <windows.h>

#include <string>

namespace remote_assist {

// 在当前线程桌面(已 SetThreadDesktop)上注入键鼠事件。
// 键盘走 scancode 路径(KEYEVENTF_SCANCODE),对锁屏/Winlogon 桌面更兼容。
class Input {
public:
    static bool SendKey(USHORT scancode, bool down, bool extended = false);
    static bool SendMouseAbs(double x, double y);
    static bool SendMouseButton(const std::string& button, bool down);
    static bool SendWheel(int delta);
};

}  // namespace remote_assist
