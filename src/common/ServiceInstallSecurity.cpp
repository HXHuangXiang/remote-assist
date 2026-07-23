#include "common/ServiceInstallSecurity.h"

#include <windows.h>
#include <aclapi.h>
#include <accctrl.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace remote_assist {

namespace {

// 服务映像目录、web/、配置和日志中的写权限都可能直接改变 LocalSystem 读取的
// 内容，因此一律拒绝。祖先目录则不同：Windows 默认根目录有时会允许普通用户
// “创建子目录/追加数据”，这不能修改已存在且受保护的 RemoteAssist 目录；只有能
// 删除子项、改变 ACL/所有者或直接写祖先本身的权限才会形成替换风险。
constexpr ACCESS_MASK kAlwaysDangerousWriteAccess =
    GENERIC_ALL | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER | DELETE |
    FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD;
constexpr ACCESS_MASK kTargetContentWriteAccess =
    FILE_WRITE_DATA | FILE_APPEND_DATA;

// 安装校验除了排除低权限可写，还必须确认真正运行服务的 LocalSystem 可以访问
// 对应资源。仅校验“所有者是管理员”会放过管理员专属 ACL：图形安装可成功，但
// LocalSystem 随后无法打开 config/.config.lock 或创建 logs，且日志自身也落不下去。
constexpr ACCESS_MASK kSystemReadExecuteAccess =
    FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
constexpr ACCESS_MASK kSystemDirectoryCreateAccess =
    FILE_GENERIC_READ | FILE_GENERIC_WRITE;
constexpr ACCESS_MASK kSystemReadWriteAccess =
    FILE_GENERIC_READ | FILE_GENERIC_WRITE;
constexpr ACCESS_MASK kSystemRuntimeFileAccess =
    FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;

bool HasDangerousWriteAccess(ACCESS_MASK access, bool ancestorDirectory) {
    if ((access & kAlwaysDangerousWriteAccess) != 0) {
        return true;
    }
    return !ancestorDirectory && (access & kTargetContentWriteAccess) != 0;
}

bool IsSamePath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring JoinPath(const std::wstring& directory, const wchar_t* name) {
    if (directory.empty() || !name || !*name) {
        return {};
    }
    if (directory.back() == L'\\' || directory.back() == L'/') {
        return directory + name;
    }
    return directory + L"\\" + name;
}

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    // C:\Program Files 的父目录是 C:\，而不是不可用于 ACL 检查的 C:。
    if (separator == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    // GetModuleFileNameW 在扩展长度路径下可返回 \\?\C:\...；该前缀下卷根的
    // 反斜杠位于索引 6，不能退化成无法用于 GetNamedSecurityInfoW 的 \\?\C:。
    if (separator == 6 && path.size() >= 7 && path.rfind(L"\\\\?\\", 0) == 0 &&
        path[5] == L':') {
        return path.substr(0, 7);
    }
    return path.substr(0, separator);
}

bool CreateKnownSid(WELL_KNOWN_SID_TYPE type, std::array<BYTE, SECURITY_MAX_SID_SIZE>& buffer,
                    PSID& sid) {
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!CreateWellKnownSid(type, nullptr, buffer.data(), &size)) {
        sid = nullptr;
        return false;
    }
    sid = buffer.data();
    return true;
}

// 避免与 Windows SDK 的全局 IsWellKnownSid 同名。这里需要的是“该 SID 是否等于
// 指定已知 SID”，而不是 SDK API 的“它是否属于任何已知 SID”语义。
bool IsExpectedWellKnownSid(PSID sid, WELL_KNOWN_SID_TYPE type) {
    if (!sid || !IsValidSid(sid)) {
        return false;
    }
    std::array<BYTE, SECURITY_MAX_SID_SIZE> buffer{};
    PSID expected = nullptr;
    return CreateKnownSid(type, buffer, expected) && EqualSid(sid, expected) != FALSE;
}

class InstallerPrincipal {
public:
    bool Load(std::wstring& reason) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            reason = L"无法读取安装程序的访问令牌。";
            return false;
        }

        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> userBuffer(size);
        bool ok = size != 0 && GetTokenInformation(token, TokenUser, userBuffer.data(), size, &size);
        if (!ok) {
            CloseHandle(token);
            reason = L"无法读取安装程序所属用户。";
            return false;
        }

        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
        const DWORD sidSize = GetLengthSid(tokenUser->User.Sid);
        userSid_.resize(sidSize);
        ok = sidSize != 0 && CopySid(sidSize, userSid_.data(), tokenUser->User.Sid) != FALSE;

        std::array<BYTE, SECURITY_MAX_SID_SIZE> administratorsBuffer{};
        PSID administratorsSid = nullptr;
        BOOL isAdministrator = FALSE;
        if (ok && CreateKnownSid(WinBuiltinAdministratorsSid, administratorsBuffer,
                                 administratorsSid)) {
            CheckTokenMembership(token, administratorsSid, &isAdministrator);
        }
        CloseHandle(token);
        if (!ok) {
            reason = L"无法复制安装程序所属用户。";
            return false;
        }
        currentUserIsAdministrator_ = isAdministrator != FALSE;
        return true;
    }

    bool IsTrustedControlSid(PSID sid) const {
        if (!sid || !IsValidSid(sid)) {
            return false;
        }
        if (IsExpectedWellKnownSid(sid, WinLocalSystemSid) ||
            IsExpectedWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
            return true;
        }
        // 允许当前已提升的管理员帐户自身拥有安装目录。管理员本身已经具备提升
        // 权限，不属于本检查要阻止的“普通帐户可改写高权限服务”情形。
        return currentUserIsAdministrator_ && !userSid_.empty() &&
            EqualSid(sid, reinterpret_cast<PSID>(const_cast<BYTE*>(userSid_.data()))) != FALSE;
    }

private:
    std::vector<BYTE> userSid_;
    bool currentUserIsAdministrator_ = false;
};

std::wstring AccountNameForSid(PSID sid) {
    if (!sid || !IsValidSid(sid)) {
        return L"未知身份";
    }
    DWORD accountChars = 0;
    DWORD domainChars = 0;
    SID_NAME_USE use{};
    LookupAccountSidW(nullptr, sid, nullptr, &accountChars, nullptr, &domainChars, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || accountChars == 0) {
        return L"未知身份";
    }
    std::vector<wchar_t> account(accountChars, L'\0');
    std::vector<wchar_t> domain(domainChars, L'\0');
    if (!LookupAccountSidW(nullptr, sid, account.data(), &accountChars,
                           domain.data(), &domainChars, &use)) {
        return L"未知身份";
    }
    const std::wstring accountName(account.data());
    if (domain.empty() || domain[0] == L'\0') {
        return accountName;
    }
    return std::wstring(domain.data()) + L"\\" + accountName;
}

bool IsStandardAllowedAce(BYTE aceType) {
    return aceType == ACCESS_ALLOWED_ACE_TYPE ||
        aceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE;
}

bool HasRequiredLocalSystemAccess(PACL dacl, ACCESS_MASK requiredAccess,
                                  const std::wstring& path, std::wstring& reason) {
    if (requiredAccess == 0) {
        return true;
    }
    std::array<BYTE, SECURITY_MAX_SID_SIZE> sidBuffer{};
    PSID systemSid = nullptr;
    if (!CreateKnownSid(WinLocalSystemSid, sidBuffer, systemSid)) {
        reason = L"无法创建 LocalSystem SID 用于 ACL 校验。";
        return false;
    }
    TRUSTEE_W trustee{};
    BuildTrusteeWithSidW(&trustee, systemSid);
    ACCESS_MASK effectiveRights = 0;
    if (GetEffectiveRightsFromAclW(dacl, &trustee, &effectiveRights) != ERROR_SUCCESS) {
        reason = L"无法计算 LocalSystem 的有效 ACL 权限：" + path;
        return false;
    }

    // GetEffectiveRightsFromAclW 会保留 DACL 中的 GENERIC_* 位。将两侧映射为
    // 文件对象的具体访问位后再比较，避免“ACL 写 GENERIC_READ、需求写
    // FILE_GENERIC_READ”时发生错误拒绝。
    GENERIC_MAPPING mapping{};
    mapping.GenericRead = FILE_GENERIC_READ;
    mapping.GenericWrite = FILE_GENERIC_WRITE;
    mapping.GenericExecute = FILE_GENERIC_EXECUTE;
    mapping.GenericAll = FILE_ALL_ACCESS;
    MapGenericMask(&effectiveRights, &mapping);
    MapGenericMask(&requiredAccess, &mapping);
    if ((effectiveRights & requiredAccess) == requiredAccess) {
        return true;
    }
    reason = L"LocalSystem 对服务运行资源权限不足：" + path;
    return false;
}

bool ValidateDacl(const std::wstring& path, const InstallerPrincipal& principal,
                  bool ancestorDirectory, ACCESS_MASK requiredSystemAccess,
                  std::wstring& reason) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD result = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS) {
        reason = L"无法读取路径的 ACL：" + path;
        return false;
    }

    const auto releaseDescriptor = [&] {
        if (descriptor) {
            LocalFree(descriptor);
            descriptor = nullptr;
        }
    };

    if (!owner || !principal.IsTrustedControlSid(owner)) {
        reason = L"路径的所有者不是 LocalSystem 或管理员：" + path;
        releaseDescriptor();
        return false;
    }
    if (!dacl) {
        reason = L"路径使用空 DACL，所有帐户均可改写：" + path;
        releaseDescriptor();
        return false;
    }
    if (!HasRequiredLocalSystemAccess(dacl, requiredSystemAccess, path, reason)) {
        releaseDescriptor();
        return false;
    }

    // 先按常见的低权限组计算有效权限。即使 ACL 中存在 Deny/Allow 的组合，也不
    // 依赖 ACE 的书写顺序直接猜测结果。
    constexpr WELL_KNOWN_SID_TYPE kUntrustedGroups[] = {
        WinWorldSid,
        WinAuthenticatedUserSid,
        WinBuiltinUsersSid,
        WinBuiltinGuestsSid,
        WinInteractiveSid,
        WinAnonymousSid,
    };
    for (const WELL_KNOWN_SID_TYPE type : kUntrustedGroups) {
        std::array<BYTE, SECURITY_MAX_SID_SIZE> sidBuffer{};
        PSID sid = nullptr;
        if (!CreateKnownSid(type, sidBuffer, sid)) {
            reason = L"无法创建用于检查低权限 ACL 的系统 SID。";
            releaseDescriptor();
            return false;
        }
        TRUSTEE_W trustee{};
        BuildTrusteeWithSidW(&trustee, sid);
        ACCESS_MASK effectiveRights = 0;
        const DWORD rightsResult = GetEffectiveRightsFromAclW(dacl, &trustee, &effectiveRights);
        if (rightsResult != ERROR_SUCCESS) {
            reason = L"无法计算路径的有效 ACL 权限：" + path;
            releaseDescriptor();
            return false;
        }
        if (HasDangerousWriteAccess(effectiveRights, ancestorDirectory)) {
            reason = L"普通帐户组“" + AccountNameForSid(sid) +
                L"”可改写路径：" + path;
            releaseDescriptor();
            return false;
        }
    }

    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
        reason = L"无法枚举路径的 ACL：" + path;
        releaseDescriptor();
        return false;
    }
    for (DWORD index = 0; index < info.AceCount; ++index) {
        void* rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce) || !rawAce) {
            reason = L"无法读取路径的 ACL 条目：" + path;
            releaseDescriptor();
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(rawAce);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0) {
            continue;
        }
        const ACCESS_MASK mask = *reinterpret_cast<const ACCESS_MASK*>(
            static_cast<const BYTE*>(rawAce) + sizeof(ACE_HEADER));
        if (!HasDangerousWriteAccess(mask, ancestorDirectory)) {
            continue;
        }
        if (!IsStandardAllowedAce(header->AceType)) {
            // 文件系统很少使用对象型 Allow ACE；为了不把无法可靠解析的允许项
            // 误判为安全，安装服务时采用保守失败策略。
            reason = L"路径包含无法安全验证的写入 ACL 条目：" + path;
            releaseDescriptor();
            return false;
        }
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
        PSID sid = const_cast<DWORD*>(&allowed->SidStart);
        if (!principal.IsTrustedControlSid(sid)) {
            reason = L"非管理员身份“" + AccountNameForSid(sid) +
                L"”可改写路径：" + path;
            releaseDescriptor();
            return false;
        }
    }

    releaseDescriptor();
    return true;
}

bool ValidateExistingObject(const std::wstring& path, const InstallerPrincipal& principal,
                            std::wstring& reason, bool ancestorDirectory = false,
                            ACCESS_MASK requiredSystemAccess = 0) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        reason = L"安装所需路径不存在：" + path;
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        reason = L"安装路径不能包含符号链接或其他重解析点：" + path;
        return false;
    }
    return ValidateDacl(path, principal, ancestorDirectory, requiredSystemAccess, reason);
}

bool IsMissingPathError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool ValidateDirectoryTree(const std::wstring& directory, const wchar_t* resourceName,
                           const InstallerPrincipal& principal, ACCESS_MASK requiredSystemAccess,
                           std::wstring& reason) {
    if (!ValidateExistingObject(directory, principal, reason, false, requiredSystemAccess)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        reason = std::wstring(resourceName) + L"不是目录：" + directory;
        return false;
    }
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = JoinPath(directory, L"*");
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        reason = std::wstring(L"无法枚举") + resourceName + L"：" + directory;
        return false;
    }
    bool valid = true;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring child = JoinPath(directory, data.cFileName);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            reason = std::wstring(resourceName) + L"不能包含符号链接或其他重解析点：" + child;
            valid = false;
            break;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!ValidateDirectoryTree(child, resourceName, principal, requiredSystemAccess,
                                       reason)) {
                valid = false;
                break;
            }
        } else if (!ValidateExistingObject(child, principal, reason, false,
                                           requiredSystemAccess)) {
            valid = false;
            break;
        }
    } while (FindNextFileW(find, &data));
    const DWORD enumerationError = GetLastError();
    FindClose(find);
    if (valid && enumerationError != ERROR_NO_MORE_FILES) {
        reason = L"枚举" + std::wstring(resourceName) + L"时发生错误：" + directory;
        return false;
    }
    return valid;
}

// 运行产物在首次启动前可能尚不存在；存在时必须和服务映像一样可验证，缺失时则
// 由正常的配置/日志初始化流程创建。这样既不会阻止全新安装，也能拒绝预置的链接、
// 目录伪装或低权限可写文件。
bool ValidateOptionalRegularFile(const std::wstring& path, const wchar_t* resourceName,
                                 const InstallerPrincipal& principal,
                                 ACCESS_MASK requiredSystemAccess, std::wstring& reason) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (IsMissingPathError(GetLastError())) {
            return true;
        }
        reason = L"无法读取" + std::wstring(resourceName) + L"：" + path;
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        reason = std::wstring(resourceName) + L"不是普通文件：" + path;
        return false;
    }
    return ValidateExistingObject(path, principal, reason, false, requiredSystemAccess);
}

bool ValidateOptionalDirectoryTree(const std::wstring& path, const wchar_t* resourceName,
                                   const InstallerPrincipal& principal,
                                   ACCESS_MASK requiredSystemAccess, std::wstring& reason) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (IsMissingPathError(GetLastError())) {
            return true;
        }
        reason = L"无法读取" + std::wstring(resourceName) + L"：" + path;
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        reason = std::wstring(resourceName) + L"不是目录：" + path;
        return false;
    }
    return ValidateDirectoryTree(path, resourceName, principal, requiredSystemAccess, reason);
}

}  // namespace

ServiceInstallPathValidation ValidateServiceInstallPath(const std::wstring& exePath) {
    ServiceInstallPathValidation validation;
    if (exePath.empty()) {
        validation.reason = L"无法解析 RemoteAssist.exe 的路径。";
        return validation;
    }
    const std::wstring exeDirectory = ParentDirectory(exePath);
    if (exeDirectory.empty()) {
        validation.reason = L"无法解析 RemoteAssist.exe 所在目录。";
        return validation;
    }

    wchar_t volumeRoot[MAX_PATH] = {};
    if (!GetVolumePathNameW(exePath.c_str(), volumeRoot, MAX_PATH)) {
        validation.reason = L"无法解析安装目录所属卷。";
        return validation;
    }
    const UINT driveType = GetDriveTypeW(volumeRoot);
    if (driveType != DRIVE_FIXED) {
        validation.reason = L"LocalSystem 服务只能安装在本机固定磁盘目录，不能使用网络或可移动卷。";
        return validation;
    }

    InstallerPrincipal principal;
    if (!principal.Load(validation.reason)) {
        return validation;
    }

    // 安装目录本身包含服务映像、web/ 和运行产物，必须按严格文件内容权限验证。
    // 它之上的祖先目录则只需拒绝“能替换既有子项”的权限，不能把 Windows 默认
    // 根目录的“允许创建新目录”误判为可改写本安装目录。
    if (!ValidateExistingObject(exeDirectory, principal, validation.reason, false,
                                kSystemDirectoryCreateAccess)) {
        return validation;
    }
    const DWORD directoryAttributes = GetFileAttributesW(exeDirectory.c_str());
    if ((directoryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        validation.reason = L"RemoteAssist.exe 所在路径不是目录：" + exeDirectory;
        return validation;
    }

    std::wstring current = ParentDirectory(exeDirectory);
    const std::wstring root(volumeRoot);
    if (current.empty()) {
        validation.reason = L"无法完整验证安装目录的父目录链。";
        return validation;
    }
    for (;;) {
        if (!ValidateExistingObject(current, principal, validation.reason, true)) {
            return validation;
        }
        if (IsSamePath(current, root)) {
            break;
        }
        current = ParentDirectory(current);
        if (current.empty()) {
            validation.reason = L"无法完整验证安装目录的父目录链。";
            return validation;
        }
    }

    const DWORD exeAttributes = GetFileAttributesW(exePath.c_str());
    if (exeAttributes == INVALID_FILE_ATTRIBUTES || (exeAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        validation.reason = L"RemoteAssist.exe 文件不存在或不是普通文件。";
        return validation;
    }
    if (!ValidateExistingObject(exePath, principal, validation.reason, false,
                                kSystemReadExecuteAccess)) {
        return validation;
    }
    if (!ValidateDirectoryTree(JoinPath(exeDirectory, L"web"), L"网页资源目录",
                               principal, kSystemReadExecuteAccess, validation.reason)) {
        return validation;
    }
    // 配置、日志和首次密码提示都由 LocalSystem 服务从 exe 同级目录读取或写入。
    // 新安装时这些运行产物尚不存在；若用户已运行过便携模式，则必须在注册高
    // 权限服务前确认它们同样不是链接、目录伪装或低权限可写对象。
    if (!ValidateOptionalRegularFile(JoinPath(exeDirectory, L"config.json"), L"配置文件",
                                     principal, kSystemReadExecuteAccess, validation.reason)) {
        return validation;
    }
    if (!ValidateOptionalDirectoryTree(JoinPath(exeDirectory, L"logs"), L"日志目录",
                                       principal, kSystemRuntimeFileAccess, validation.reason)) {
        return validation;
    }
    // ConfigFileLock 使用同级 .config.lock 跨 Session 排他。若便携运行遗留了由
    // 普通用户控制的锁文件，LocalSystem 会在服务启动时被拒绝或永久等待，因此必须
    // 在注册服务前和 config.json 一样验证其对象类型、重解析点与 ACL。
    if (!ValidateOptionalRegularFile(JoinPath(exeDirectory, L".config.lock"), L"配置锁文件",
                                     principal, kSystemReadWriteAccess, validation.reason)) {
        return validation;
    }
    if (!ValidateOptionalRegularFile(JoinPath(exeDirectory, L".initial-password"),
                                     L"首次密码提示文件", principal, DELETE, validation.reason)) {
        return validation;
    }

    validation.secure = true;
    validation.reason.clear();
    return validation;
}

}  // namespace remote_assist
