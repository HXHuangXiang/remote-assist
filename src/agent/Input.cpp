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
    // 所有 SendInput 与状态修改均在 mutex 内串行化。Agent 停止时先关闭该开关，
    // 再释放状态，可消除 SendInput 已执行但 ReleaseAll 尚未观察到状态的卡键竞态。
    bool accepting = false;
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

void Input::Enable() {
    std::lock_guard<std::mutex> lock(g_inputState.mutex);
    g_inputState.accepting = true;
}

void Input::Disable() {
    std::lock_guard<std::mutex> lock(g_inputState.mutex);
    g_inputState.accepting = false;
}

bool Input::SendKey(USHORT sc, bool down, bool extended) {
    std::lock_guard<std::mutex> lock(g_inputState.mutex);
    if (!g_inputState.accepting) {
        return false;
    }
    const size_t index = KeyIndex(sc, extended);
    if (g_inputState.keys[index] == down) {
        return true;
    }
    if (!SendRawKey(sc, down, extended)) {
        log::Warn("SendInput key failed: " + std::to_string(GetLastError()));
        return false;
    }
    g_inputState.keys[index] = down;
    return true;
}

bool Input::SendMouseAbs(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    std::lock_guard<std::mutex> lock(g_inputState.mutex);
    if (!g_inputState.accepting) {
        return false;
    }
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
    std::lock_guard<std::mutex> lock(g_inputState.mutex);
    if (!g_inputState.accepting) {
        return false;
    }
    if (*state == down) {
        return true;
    }
    if (!SendRawMouseButton(flag)) {
        log::Warn("SendInput mouse button failed: " + std::to_string(GetLastError()));
        return false;
    }
    *state = down;
    return true;
}

bool Input::SendWheel(int delta) {
    std::lock_guard<std::mutex> lock(g_inputState.mutex);
    if (!g_inputState.accepting) {
        return false;
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

void Input::ReleaseAll() {
    std::lock_guard<std::mutex> lock(g_inputState.mutex);
    for (size_t index = 0; index < g_inputState.keys.size(); ++index) {
        if (!g_inputState.keys[index]) {
            continue;
        }
        const bool extended = index >= 128;
        const USHORT scancode = static_cast<USHORT>(index & 0x7F);
        if (!SendRawKey(scancode, false, extended)) {
            log::Warn("SendInput key release failed: " + std::to_string(GetLastError()));
        }
        g_inputState.keys[index] = false;
    }
    if (g_inputState.leftDown && !SendRawMouseButton(MOUSEEVENTF_LEFTUP)) {
        log::Warn("SendInput left release failed: " + std::to_string(GetLastError()));
    }
    if (g_inputState.rightDown && !SendRawMouseButton(MOUSEEVENTF_RIGHTUP)) {
        log::Warn("SendInput right release failed: " + std::to_string(GetLastError()));
    }
    if (g_inputState.middleDown && !SendRawMouseButton(MOUSEEVENTF_MIDDLEUP)) {
        log::Warn("SendInput middle release failed: " + std::to_string(GetLastError()));
    }
    g_inputState.leftDown = false;
    g_inputState.rightDown = false;
    g_inputState.middleDown = false;
}

}  // namespace remote_assist
