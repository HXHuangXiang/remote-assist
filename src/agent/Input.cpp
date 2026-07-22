#include "agent/Input.h"

#include "common/Log.h"

#include <algorithm>
#include <cmath>

#pragma comment(lib, "user32.lib")

namespace remote_assist {

bool Input::SendKey(USHORT sc, bool down, bool extended) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = sc;
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (extended) {
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (!down) {
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        log::Warn("SendInput key failed: " + std::to_string(GetLastError()));
        return false;
    }
    return true;
}

bool Input::SendMouseAbs(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = static_cast<LONG>(x * 65535.0 + 0.5);
    in.mi.dy = static_cast<LONG>(y * 65535.0 + 0.5);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool Input::SendMouseButton(const std::string& button, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    DWORD flag = 0;
    if (button == "left") {
        flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    } else if (button == "right") {
        flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else if (button == "middle") {
        flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    } else {
        return false;
    }
    in.mi.dwFlags = flag;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool Input::SendWheel(int delta) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

}  // namespace remote_assist
