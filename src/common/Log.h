#pragma once

#include <string>

namespace remote_assist::log {

// 初始化日志目录(exe 同目录下的 logs)。每个进程应传入独立文件名，避免
// service、agent、tray 与配置窗口跨进程抢写同一文件。目录会被创建。
void Init(const std::wstring& dir, const wchar_t* fileName = L"remote-assist.log");

// Tray 在普通用户令牌下运行时不应取得受保护安装目录的写权限。该模式只把日志
// 发送给 LocalSystem 服务创建的受控命名管道，由服务转存为 exe 同级 logs/tray.log。
// 服务未运行时仍会输出 OutputDebugString，但不会退化为放宽目录 ACL 的本地写入。
void InitPipeSink(const std::wstring& pipeName);

// 写一条日志:同时输出到 OutputDebugString 与日志文件,线程安全。
void Write(const char* level, const std::string& msg);

// 使用当前已初始化的日志目录写入指定文件。仅服务端的受控 IPC 消费者使用，
// 以便把 Tray 的诊断与 service.log 分开保存。
void WriteToFile(const wchar_t* fileName, const char* level, const std::string& msg);

void Info(const std::string& msg);
void Warn(const std::string& msg);
void Error(const std::string& msg);

}  // namespace remote_assist::log
