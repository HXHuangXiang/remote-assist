#pragma once

#include <windows.h>

#include <string>

namespace remote_assist {

// CheckRebind 的结果必须区分“桌面未变”和“检查失败”。采集线程只需要知道是否
// 切换；输入线程在锁屏/解锁的临界窗口则必须在无法验证当前 input desktop 时拒绝
// 关键按键，不能把凭据输入到已经不可见的旧 desktop。
enum class DesktopRebindResult {
    kUnchanged,
    kRebound,
    kFailed,
};

// 绑定当前 input desktop(锁屏 Winlogon / 普通 Default),并周期重绑跟随切换。
// 由 agent(以 winlogon token 启动后)持有,注入与采集都依赖当前线程桌面绑定。
class DesktopAccess {
public:
    DesktopAccess() = default;
    ~DesktopAccess();

    DesktopAccess(const DesktopAccess&) = delete;
    DesktopAccess& operator=(const DesktopAccess&) = delete;

    // 绑定到当前 input desktop。失败返回 false。
    bool Bind();

    // 若当前 input desktop 与上次绑定不同,重新 SetThreadDesktop。调用方可区分
    // 桌面未变、成功重绑和无法验证当前桌面的失败。
    DesktopRebindResult CheckRebind();

    // 当前线程是否已经成功绑定桌面。
    bool IsBound() const { return h_desk_ != nullptr; }

    // 当前桌面名(Winlogon / Default 等),仅用于日志/状态展示。
    std::wstring CurrentName() const;

private:
    HDESK h_desk_ = nullptr;
    DWORD owner_thread_id_ = 0;
};

}  // namespace remote_assist
