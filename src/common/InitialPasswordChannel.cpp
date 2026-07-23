#include "common/InitialPasswordChannel.h"

#include "common/Log.h"

#include <bcrypt.h>
#include <sddl.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace remote_assist {

namespace {

constexpr uint32_t kChannelMagic = 0x52504153;  // "RPAS"
constexpr size_t kMaxPasswordBytes = 3072;
constexpr size_t kChannelBytes = sizeof(uint32_t) * 2 + kMaxPasswordBytes;
constexpr int kNameCreateAttempts = 4;

struct ChannelPayload {
    uint32_t magic = kChannelMagic;
    uint32_t passwordBytes = 0;
    char password[kMaxPasswordBytes] = {};
};
static_assert(sizeof(ChannelPayload) == kChannelBytes,
              "initial password channel payload size must remain stable");

std::wstring RandomChannelName() {
    std::array<uint8_t, 16> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring name = L"Global\\RemoteAssistInitialPassword-";
    name.reserve(name.size() + nonce.size() * 2);
    for (const uint8_t byte : nonce) {
        name.push_back(kHex[(byte >> 4) & 0x0F]);
        name.push_back(kHex[byte & 0x0F]);
    }
    return name;
}

bool BuildChannelSecurityAttributes(const std::wstring& readerUserSid,
                                    SECURITY_ATTRIBUTES& attributes,
                                    PSECURITY_DESCRIPTOR& descriptor) {
    descriptor = nullptr;
    // SDDL 中动态拼接 SID 前先让系统解析一次，既校验调用方传入的身份，也避免
    // 非法字符串改变后续 DACL 的语义。当前 Service 从 WTS 用户令牌取得该值。
    PSID parsedSid = nullptr;
    if (readerUserSid.empty() ||
        !ConvertStringSidToSidW(readerUserSid.c_str(), &parsedSid)) {
        log::Error("initial password channel reader SID invalid: " +
                   std::to_string(GetLastError()));
        return false;
    }
    LocalFree(parsedSid);

    // 共享内存名称会作为 Tray 启动参数传递，不能把“名称随机”当成授权边界。显式
    // 仅允许 LocalSystem、管理员和目标 Tray 用户读取；不再授予所有交互用户 IU。
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;" +
        readerUserSid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        log::Error("initial password channel security descriptor failed: " +
                   std::to_string(GetLastError()));
        return false;
    }
    attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    return true;
}

}  // namespace

InitialPasswordChannel::~InitialPasswordChannel() {
    Close();
}

bool InitialPasswordChannel::Create(const std::string& password,
                                    const std::wstring& readerUserSid,
                                    std::wstring& name) {
    Close();
    name.clear();
    if (password.empty() || password.size() > kMaxPasswordBytes) {
        log::Warn("initial password channel rejected invalid password length");
        return false;
    }

    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!BuildChannelSecurityAttributes(readerUserSid, attributes, descriptor)) {
        return false;
    }

    for (int attempt = 0; attempt < kNameCreateAttempts; ++attempt) {
        const std::wstring candidate = RandomChannelName();
        if (candidate.empty()) {
            log::Error("initial password channel random name generation failed");
            break;
        }
        SetLastError(ERROR_SUCCESS);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE,
                                             0, static_cast<DWORD>(kChannelBytes),
                                             candidate.c_str());
        if (!mapping) {
            log::Error("initial password channel creation failed: " +
                       std::to_string(GetLastError()));
            break;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mapping);
            continue;
        }

        auto* payload = static_cast<ChannelPayload*>(
            MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kChannelBytes));
        if (!payload) {
            log::Error("initial password channel mapping failed: " +
                       std::to_string(GetLastError()));
            CloseHandle(mapping);
            break;
        }
        SecureZeroMemory(payload, kChannelBytes);
        payload->magic = kChannelMagic;
        payload->passwordBytes = static_cast<uint32_t>(password.size());
        std::memcpy(payload->password, password.data(), password.size());
        UnmapViewOfFile(payload);
        mapping_ = mapping;
        name = candidate;
        LocalFree(descriptor);
        return true;
    }

    LocalFree(descriptor);
    return false;
}

void InitialPasswordChannel::Close() {
    if (!mapping_) {
        return;
    }
    if (auto* payload = static_cast<ChannelPayload*>(
            MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, kChannelBytes))) {
        SecureZeroMemory(payload, kChannelBytes);
        UnmapViewOfFile(payload);
    }
    CloseHandle(mapping_);
    mapping_ = nullptr;
}

bool ReadInitialPasswordChannel(const std::wstring& name, std::string& password) {
    password.clear();
    if (name.empty()) {
        return false;
    }
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) {
        log::Warn("initial password channel open failed: " + std::to_string(GetLastError()));
        return false;
    }
    const auto* payload = static_cast<const ChannelPayload*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, kChannelBytes));
    if (!payload) {
        log::Warn("initial password channel read mapping failed: " +
                  std::to_string(GetLastError()));
        CloseHandle(mapping);
        return false;
    }

    const bool valid = payload->magic == kChannelMagic && payload->passwordBytes > 0 &&
        payload->passwordBytes <= kMaxPasswordBytes;
    if (valid) {
        password.assign(payload->password, payload->password + payload->passwordBytes);
    } else {
        log::Warn("initial password channel contained invalid data");
    }
    UnmapViewOfFile(payload);
    CloseHandle(mapping);
    return valid;
}

}  // namespace remote_assist
