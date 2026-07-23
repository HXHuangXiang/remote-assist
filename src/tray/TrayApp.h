#pragma once

#include <windows.h>

#include <string>
#include <shellapi.h>

namespace remote_assist {

// 用户会话托盘 UI。由 service 用 explorer token 启动;负责展示首次生成的访问密码、
// 提示服务运行状态、提供退出入口。不做隐藏运行。
class TrayApp {
public:
    TrayApp() = default;
    ~TrayApp();

    // 进入消息循环,阻塞直到用户选择退出。
    // initialPasswordChannel 仅由 Service 首次启动 Tray 时传入，名称随机且不包含
    // 明文密码；Tray 读取后保存在本进程内，供用户通过菜单查看一次。
    int Run(const std::wstring& initialPasswordChannel = {});

private:
    // 创建隐藏消息窗口和通知区域图标；任一步失败均返回 false，供服务监控重试。
    bool CreateIcon();
    bool AddIconToTaskbar();
    void ShowMenu();
    void OpenSetupWindow();
    void ShowPasswordDialog();
    void RemoveIcon();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    HMENU hMenu_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    std::wstring password_;  // 从一次性内存通道读取的明文，仅保留在 Tray 进程内
    HANDLE instanceMutex_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool iconAdded_ = false;
};

}  // namespace remote_assist
