#pragma once

#include <windows.h>

#include <string>

namespace remote_assist {

// Service 首次生成随机访问密码后，通过随机命名的只读共享内存把它短暂交给当前
// 用户会话的 Tray。明文不会落入 exe 目录，Tray 也不需要取得配置目录写权限。
class InitialPasswordChannel {
public:
    InitialPasswordChannel() = default;
    ~InitialPasswordChannel();

    InitialPasswordChannel(const InitialPasswordChannel&) = delete;
    InitialPasswordChannel& operator=(const InitialPasswordChannel&) = delete;

    // 创建含 UTF-8 密码的共享内存，成功时返回随机对象名。readerUserSid 必须是
    // 将运行 Tray 的交互用户 SID 字符串；只有该用户可读，避免同机其他登录用户
    // 借由通道名称读取首次密码。调用方必须在交付窗口结束后调用 Close()，以擦除
    // 内存并释放命名对象。
    bool Create(const std::string& password, const std::wstring& readerUserSid,
                std::wstring& name);
    bool IsOpen() const { return mapping_ != nullptr; }
    void Close();

private:
    HANDLE mapping_ = nullptr;
};

// Tray 以只读方式读取 Service 创建的通道。返回 false 表示对象不存在、ACL 拒绝或
// 内容损坏；password 仅在成功时写入。
bool ReadInitialPasswordChannel(const std::wstring& name, std::string& password);

}  // namespace remote_assist
