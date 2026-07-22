#pragma once

#include <windows.h>

#include <string>

namespace remote_assist {

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

    // 若当前 input desktop 与上次绑定不同,重新 SetThreadDesktop。返回是否发生重绑。
    bool CheckRebind();

    // 当前线程是否已经成功绑定桌面。
    bool IsBound() const { return h_desk_ != nullptr; }

    // 当前桌面名(Winlogon / Default 等),仅用于日志/状态展示。
    std::wstring CurrentName() const;

private:
    HDESK h_desk_ = nullptr;
    DWORD owner_thread_id_ = 0;
    std::wstring desktop_name_;
};

}  // namespace remote_assist
