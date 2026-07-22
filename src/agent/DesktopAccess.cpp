#include "agent/DesktopAccess.h"

#include "common/Log.h"

#include <string>

namespace remote_assist {

namespace {

std::wstring DeskName(HDESK h) {
    if (!h) {
        return L"<null>";
    }
    wchar_t buf[256] = {};
    DWORD len = 0;
    if (!GetUserObjectInformationW(h, UOI_NAME, buf, sizeof(buf), &len)) {
        return L"<unknown>";
    }
    return std::wstring(buf);
}

std::string Narrow(const std::wstring& w) {
    return std::string(w.begin(), w.end());
}

}  // namespace

DesktopAccess::~DesktopAccess() {
    if (h_desk_) {
        CloseDesktop(h_desk_);
        h_desk_ = nullptr;
    }
}

bool DesktopAccess::Bind() {
    const DWORD currentThreadId = GetCurrentThreadId();
    if (h_desk_ && owner_thread_id_ != currentThreadId) {
        log::Error("DesktopAccess used from a different thread");
        return false;
    }

    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!desk) {
        log::Error("OpenInputDesktop failed: " + std::to_string(GetLastError()));
        return false;
    }
    if (!SetThreadDesktop(desk)) {
        log::Error("SetThreadDesktop failed: " + std::to_string(GetLastError()));
        CloseDesktop(desk);
        return false;
    }

    if (h_desk_) {
        CloseDesktop(h_desk_);
    }
    h_desk_ = desk;
    owner_thread_id_ = currentThreadId;
    desktop_name_ = DeskName(h_desk_);
    log::Info("bound desktop: " + Narrow(CurrentName()));
    return true;
}

bool DesktopAccess::CheckRebind() {
    const DWORD currentThreadId = GetCurrentThreadId();
    if (h_desk_ && owner_thread_id_ != currentThreadId) {
        log::Error("DesktopAccess rebind requested from a different thread");
        return false;
    }
    if (!h_desk_) {
        return Bind();
    }

    const HDESK now = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!now) {
        return false;
    }

    // OpenInputDesktop 每次都会返回新的 handle,不能直接比较 handle 值。
    // 使用桌面名判断是否发生了 Default/Winlogon 等实际切换。
    const std::wstring nowName = DeskName(now);
    if (nowName == desktop_name_) {
        CloseDesktop(now);
        return false;
    }
    if (!SetThreadDesktop(now)) {
        log::Warn("SetThreadDesktop on rebind failed: " + std::to_string(GetLastError()));
        CloseDesktop(now);
        return false;
    }
    if (h_desk_) {
        CloseDesktop(h_desk_);
    }
    h_desk_ = now;
    owner_thread_id_ = currentThreadId;
    desktop_name_ = nowName;
    log::Info("rebound desktop: " + Narrow(CurrentName()));
    return true;
}

std::wstring DesktopAccess::CurrentName() const {
    return DeskName(h_desk_);
}

}  // namespace remote_assist
