#pragma once

#include <string>

namespace remote_assist {

// 返回当前可执行文件的绝对路径。内部按需扩容，支持超过 MAX_PATH 的便携部署目录；
// 失败返回空字符串，由调用方记录其所属流程的上下文错误。
std::wstring ModulePath();

// 返回当前可执行文件所在目录；无法解析模块路径时返回空字符串。
std::wstring ModuleDirectory();

// 规范化为绝对路径。用于兼容旧安装布局的相对回退路径，失败返回空字符串。
std::wstring AbsolutePath(const std::wstring& path);

}  // namespace remote_assist
