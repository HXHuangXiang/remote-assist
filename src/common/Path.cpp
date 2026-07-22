#include "common/Path.h"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace remote_assist {

namespace {

constexpr DWORD kInitialPathCapacity = MAX_PATH;
constexpr DWORD kMaximumPathCapacity = 32768;

}  // namespace

std::string Utf8FromWide(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring ModulePath() {
    DWORD capacity = kInitialPathCapacity;
    while (capacity <= kMaximumPathCapacity) {
        std::vector<wchar_t> buffer(capacity, L'\0');
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), capacity);
        if (copied == 0) {
            return {};
        }
        // Windows 10+ 成功时返回不含终止符的长度；截断时返回 nSize。路径恰好
        // 占用 capacity - 1 个字符仍是合法结果，不能把它误判为失败。
        if (copied < capacity) {
            return std::wstring(buffer.data(), copied);
        }
        if (capacity == kMaximumPathCapacity) {
            return {};
        }
        capacity = std::min<DWORD>(kMaximumPathCapacity, capacity * 2);
    }
    return {};
}

std::wstring ModuleDirectory() {
    const std::wstring modulePath = ModulePath();
    if (modulePath.empty()) {
        return {};
    }
    const size_t separator = modulePath.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : modulePath.substr(0, separator);
}

std::wstring AbsolutePath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    // 不依赖 nBufferLength=0 的查询语义。缓冲不足时 GetFullPathNameW 返回所需
    // 容量（含终止符），据此扩容；成功时返回不含终止符的实际长度。
    DWORD capacity = kInitialPathCapacity;
    while (capacity <= kMaximumPathCapacity) {
        std::vector<wchar_t> buffer(capacity, L'\0');
        const DWORD copied = GetFullPathNameW(path.c_str(), capacity, buffer.data(), nullptr);
        if (copied == 0) {
            return {};
        }
        if (copied < capacity) {
            return std::wstring(buffer.data(), copied);
        }
        if (copied > kMaximumPathCapacity || capacity == kMaximumPathCapacity) {
            return {};
        }
        // copied 是 Windows 返回的所需容量；再与倍增目标取较大值，既避免小步
        // 重试，也处理恰好需要 32768 个 wchar_t（含终止符）的合法边界。
        capacity = std::min<DWORD>(kMaximumPathCapacity,
            std::max<DWORD>(capacity * 2, copied));
    }
    return {};
}

}  // namespace remote_assist
