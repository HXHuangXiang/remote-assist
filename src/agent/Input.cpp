#include "agent/Input.h"

#include "common/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

#pragma comment(lib, "user32.lib")

namespace remote_assist {

namespace {

struct InputState {
    // 下标 0..127 是普通键，128..255 是扩展键。
    std::array<bool, 256> keys{};
    bool leftDown = false;
    bool rightDown = false;
    bool middleDown = false;
    std::mutex mutex;
};

InputState g_inputState;

size_t KeyIndex(USHORT scancode, bool extended) {
    return static_cast<size_t>(scancode & 0x7F) + (extended ? 128 : 0);
}

bool SendRawKey(USHORT scancode, bool down, bool extended) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scancode;
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (extended) {
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (!down) {
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool SendRawMouseButton(DWORD flag) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

}  // namespace

bool Input::SendKey(USHORT sc, bool down, bool extended) {
    if (!SendRawKey(sc, down, extended)) {
        log::Warn("SendInput key failed: " + std::to_string(GetLastError()));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_inputState.mutex);
        g_inputState.keys[KeyIndex(sc, extended)] = down;
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
    DWORD flag = 0;
    bool* state = nullptr;
    if (button == "left") {
        flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        state = &g_inputState.leftDown;
    } else if (button == "right") {
        flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        state = &g_inputState.rightDown;
    } else if (button == "middle") {
        flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        state = &g_inputState.middleDown;
    } else {
        return false;
    }
    if (!SendRawMouseButton(flag)) {
        log::Warn("SendInput mouse button failed: " + std::to_string(GetLastError()));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_inputState.mutex);
        *state = down;
    }
    return true;
}

bool Input::SendWheel(int delta) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

void Input::ReleaseAll() {
    std::array<bool, 256> keys{};
    bool leftDown = false;
    bool rightDown = false;
    bool middleDown = false;
    {
        std::lock_guard<std::mutex> lock(g_inputState.mutex);
        keys = g_inputState.keys;
        leftDown = g_inputState.leftDown;
        rightDown = g_inputState.rightDown;
        middleDown = g_inputState.middleDown;
        g_inputState.keys.fill(false);
        g_inputState.leftDown = false;
        g_inputState.rightDown = false;
        g_inputState.middleDown = false;
    }

    for (size_t index = 0; index < keys.size(); ++index) {
        if (!keys[index]) {
            continue;
        }
        const bool extended = index >= 128;
        const USHORT scancode = static_cast<USHORT>(index & 0x7F);
        if (!SendRawKey(scancode, false, extended)) {
            log::Warn("SendInput key release failed: " + std::to_string(GetLastError()));
        }
    }
    if (leftDown && !SendRawMouseButton(MOUSEEVENTF_LEFTUP)) {
        log::Warn("SendInput left release failed: " + std::to_string(GetLastError()));
    }
    if (rightDown && !SendRawMouseButton(MOUSEEVENTF_RIGHTUP)) {
        log::Warn("SendInput right release failed: " + std::to_string(GetLastError()));
    }
    if (middleDown && !SendRawMouseButton(MOUSEEVENTF_MIDDLEUP)) {
        log::Warn("SendInput middle release failed: " + std::to_string(GetLastError()));
    }
}

}  // namespace remote_assist
