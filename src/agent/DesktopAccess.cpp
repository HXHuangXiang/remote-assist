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
    h_desk_ = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!h_desk_) {
        log::Error("OpenInputDesktop failed: " + std::to_string(GetLastError()));
        return false;
    }
    if (!SetThreadDesktop(h_desk_)) {
        log::Error("SetThreadDesktop failed: " + std::to_string(GetLastError()));
        CloseDesktop(h_desk_);
        h_desk_ = nullptr;
        return false;
    }
    log::Info("bound desktop: " + Narrow(CurrentName()));
    return true;
}

bool DesktopAccess::CheckRebind() {
    const HDESK now = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!now) {
        return false;
    }
    if (now == h_desk_) {
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
    log::Info("rebound desktop: " + Narrow(CurrentName()));
    return true;
}

std::wstring DesktopAccess::CurrentName() const {
    return DeskName(h_desk_);
}

}  // namespace remote_assist

