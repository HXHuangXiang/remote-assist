#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace remote_assist {

// 用户会话托盘 UI。由 service 用 explorer token 启动;负责展示首次生成的访问密码、
// 提示服务运行状态、提供退出入口。不做隐藏运行。
class TrayApp {
public:
    TrayApp() = default;
    ~TrayApp();

    // 进入消息循环,阻塞直到用户选择退出。
    int Run();

private:
    void CreateIcon();
    void ShowMenu();
    void OpenSetupWindow();
    void ShowPasswordDialog();
    void RemoveIcon();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    HMENU hMenu_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    std::wstring password_;  // 从 .initial-password 读到的明文,读后文件删除
    HANDLE instanceMutex_ = nullptr;
};

}  // namespace remote_assist
