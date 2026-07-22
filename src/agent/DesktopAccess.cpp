#include "agent/DesktopAccess.h"

#include "common/Log.h"
#include "common/Path.h"

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

// CompareObjectHandles 在部分 Windows SDK 的 import library 中没有导出，直接调用
// 会让链接阶段失败。运行时从 user32 解析可保留 Windows 10+ 上的精确比较；极旧
// 系统没有该 API 时降级为名称比较，项目的最低运行版本仍为 Windows 10。
bool IsSameDesktopObject(HDESK left, HDESK right) {
    using CompareObjectHandlesFn = BOOL(WINAPI*)(HANDLE, HANDLE);
    static const auto compare = []() -> CompareObjectHandlesFn {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<CompareObjectHandlesFn>(
                            GetProcAddress(user32, "CompareObjectHandles"))
                      : nullptr;
    }();
    if (compare) {
        return compare(left, right) != FALSE;
    }
    return DeskName(left) == DeskName(right);
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
    log::Info("bound desktop: " + Utf8FromWide(CurrentName()));
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

    // OpenInputDesktop 每次都会返回新的 handle，不能直接比较句柄值；桌面名
    // 也不足以区分“用户注销后新建的 Default”。Windows 10+ 通过底层 user
    // object 比较精确识别同名 desktop 实例替换。
    const bool sameDesktop = IsSameDesktopObject(h_desk_, now);
    if (sameDesktop) {
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
    log::Info("rebound desktop: " + Utf8FromWide(CurrentName()));
    return true;
}

std::wstring DesktopAccess::CurrentName() const {
    return DeskName(h_desk_);
}

}  // namespace remote_assist
