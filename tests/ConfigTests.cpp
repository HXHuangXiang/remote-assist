#include "common/Config.h"
#include "common/Path.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

bool Expect(bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string ToHex(const uint8_t* data, size_t length) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (size_t index = 0; index < length; ++index) {
        result.push_back(kHex[(data[index] >> 4) & 0xF]);
        result.push_back(kHex[data[index] & 0xF]);
    }
    return result;
}

// 构造历史版本的 SHA-256(salt || password)，用于验证真实的升级路径，而非只
// 断言 password_iterations 字段变化。生产代码不会公开旧算法的写入接口。
std::string LegacySha256(const std::string& saltHex, const std::string& password) {
    if (saltHex.size() != 32) {
        return {};
    }
    std::vector<uint8_t> input;
    input.reserve(16 + password.size());
    for (size_t index = 0; index < saltHex.size(); index += 2) {
        const int high = HexValue(saltHex[index]);
        const int low = HexValue(saltHex[index + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        input.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    input.insert(input.end(), password.begin(), password.end());

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32]{};
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status == 0) {
        status = BCryptHashData(hash, input.data(), static_cast<ULONG>(input.size()), 0);
    }
    if (status == 0) {
        status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0 ? ToHex(digest, sizeof(digest)) : std::string();
}

// 构造早期 PBKDF2 配置，确认升级程序不会把已发布的较低迭代参数误判为损坏配置。
std::string Pbkdf2Sha256(const std::string& saltHex, const std::string& password,
                         int iterations) {
    if (saltHex.size() != 32 || iterations <= 0) {
        return {};
    }
    std::vector<uint8_t> salt;
    salt.reserve(saltHex.size() / 2);
    for (size_t index = 0; index < saltHex.size(); index += 2) {
        const int high = HexValue(saltHex[index]);
        const int low = HexValue(saltHex[index + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        salt.push_back(static_cast<uint8_t>((high << 4) | low));
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return {};
    }
    uint8_t digest[32]{};
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()), salt.data(), static_cast<ULONG>(salt.size()),
        static_cast<ULONGLONG>(iterations), digest, sizeof(digest), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0 ? ToHex(digest, sizeof(digest)) : std::string();
}

class TestConfigFiles final {
public:
    TestConfigFiles() : configPath_(remote_assist::ConfigFilePath()) {
        // CMake 将该测试输出到独立 tests 子目录。拒绝在其他位置运行，避免手动
        // 执行测试时意外删除用户实际部署目录的 config.json。
        const std::wstring modulePath = remote_assist::ModulePath();
        safe_ = modulePath.find(L"\\tests\\") != std::wstring::npos ||
            modulePath.find(L"/tests/") != std::wstring::npos;
        if (safe_) {
            DeleteFileW(configPath_.c_str());
            DeleteFileW((remote_assist::ConfigDir() + L"\\.config.lock").c_str());
        }
    }

    ~TestConfigFiles() {
        if (safe_) {
            DeleteFileW(configPath_.c_str());
            DeleteFileW((remote_assist::ConfigDir() + L"\\.config.lock").c_str());
        }
    }

    bool IsSafe() const { return safe_; }

private:
    std::wstring configPath_;
    bool safe_ = false;
};

}  // namespace

int main() {
    TestConfigFiles files;
    bool ok = Expect(files.IsSafe(), "config test must run from CMake's isolated tests directory");
    if (!ok) {
        return 1;
    }

    constexpr char kFirstPassword[] = "config-test-first-password";
    constexpr char kSecondPassword[] = "config-test-second-password";
    remote_assist::Config cfg = remote_assist::LoadOrCreateConfig();
    ok &= Expect(!cfg.passwordHash.empty() && !cfg.salt.empty(),
                 "default configuration should create password storage");
    ok &= Expect(remote_assist::VerifyPassword(cfg, cfg.initialPassword),
                 "generated initial password should verify");

    const std::string originalSalt = cfg.salt;
    ok &= Expect(remote_assist::SetPassword(cfg, kFirstPassword),
                 "setting a PBKDF2 password should succeed");
    ok &= Expect(cfg.salt != originalSalt, "changing password must create a fresh salt");
    ok &= Expect(remote_assist::VerifyPassword(cfg, kFirstPassword),
                 "new PBKDF2 password should verify");
    ok &= Expect(!remote_assist::VerifyPassword(cfg, kSecondPassword),
                 "incorrect PBKDF2 password should be rejected");

    remote_assist::Config legacy = cfg;
    legacy.salt = "00112233445566778899aabbccddeeff";
    legacy.passwordIterations = 0;
    legacy.passwordHash = LegacySha256(legacy.salt, kSecondPassword);
    ok &= Expect(!legacy.passwordHash.empty() && remote_assist::SaveConfig(legacy),
                 "test legacy SHA-256 configuration should save");
    ok &= Expect(remote_assist::VerifyAndUpgradePassword(legacy, kSecondPassword),
                 "valid legacy password should authenticate and migrate");
    ok &= Expect(legacy.passwordIterations > 0 && legacy.salt != "00112233445566778899aabbccddeeff",
                 "legacy migration should use PBKDF2 and a new salt");
    ok &= Expect(remote_assist::VerifyPassword(legacy, kSecondPassword),
                 "migrated password should verify through PBKDF2");

    constexpr int kHistoricalPbkdf2Iterations = 100'000;
    remote_assist::Config historicalPbkdf2 = cfg;
    historicalPbkdf2.salt = "102132435465768798a9babcbddceeff";
    historicalPbkdf2.passwordIterations = kHistoricalPbkdf2Iterations;
    historicalPbkdf2.passwordHash = Pbkdf2Sha256(historicalPbkdf2.salt, kFirstPassword,
                                                  historicalPbkdf2.passwordIterations);
    ok &= Expect(!historicalPbkdf2.passwordHash.empty() &&
                     remote_assist::SaveConfig(historicalPbkdf2),
                 "historical PBKDF2 configuration should remain loadable");
    ok &= Expect(remote_assist::VerifyAndUpgradePassword(historicalPbkdf2, kFirstPassword),
                 "valid historical PBKDF2 password should authenticate and migrate");
    ok &= Expect(historicalPbkdf2.passwordIterations > kHistoricalPbkdf2Iterations &&
                     historicalPbkdf2.salt != "102132435465768798a9babcbddceeff",
                 "historical PBKDF2 migration should raise iterations and rotate salt");
    ok &= Expect(remote_assist::VerifyPassword(historicalPbkdf2, kFirstPassword),
                 "upgraded historical PBKDF2 password should verify");

    remote_assist::Config persisted;
    ok &= Expect(remote_assist::LoadConfigReadOnly(persisted),
                 "migrated configuration should be readable from disk");
    ok &= Expect(persisted.passwordIterations == historicalPbkdf2.passwordIterations &&
                     persisted.salt == historicalPbkdf2.salt &&
                     persisted.passwordHash == historicalPbkdf2.passwordHash,
                 "disk configuration should match the migrated password storage");
    return ok ? 0 : 1;
}
