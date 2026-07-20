#pragma once

#include <string>

namespace remote_assist::log {

// 初始化日志目录(如 %ProgramData%\RemoteAssist\logs)。目录会被创建。
void Init(const std::wstring& dir);

// 写一条日志:同时输出到 OutputDebugString 与日志文件,线程安全。
void Write(const char* level, const std::string& msg);

void Info(const std::string& msg);
void Warn(const std::string& msg);
void Error(const std::string& msg);

}  // namespace remote_assist::log

