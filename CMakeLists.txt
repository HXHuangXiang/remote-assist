cmake_minimum_required(VERSION 3.21)
project(RemoteAssist VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "RemoteAssist 仅支持 Windows 平台。")
endif()

# 预处理宏:Windows 10+、Unicode、精简 Windows 头、关闭 httplib 的 OpenSSL 支持。
add_compile_definitions(
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS
)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

set(SOURCES
    src/main.cpp
    src/common/Config.cpp
    src/common/Log.cpp
    src/service/ProcessLauncher.cpp
    src/service/ServiceHost.cpp
    src/agent/DesktopAccess.cpp
    src/setup/SetupDialog.cpp
    src/agent/Capture.cpp
    src/agent/EncoderMf.cpp
    src/agent/Input.cpp
    src/agent/Agent.cpp
    src/net/HttpWsServer.cpp
    src/tray/TrayApp.cpp
)

# WIN32 子系统:服务/agent/tray 都不需要控制台窗口,日志走文件。
add_executable(RemoteAssist WIN32 ${SOURCES})

target_link_libraries(RemoteAssist PRIVATE
    advapi32 user32 gdi32 dwmapi
    wtsapi32 userenv
    ws2_32
    ole32 oleaut32
    d3d11 dxgi dxguid
    windowscodecs
    shlwapi
)

if(MSVC)
    target_compile_options(RemoteAssist PRIVATE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
    )
    # 静态 CRT,目标单 exe < 8MB,无 vcredist 依赖。
    set_property(TARGET RemoteAssist PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(RemoteAssist PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS RemoteAssist RUNTIME DESTINATION bin)
install(DIRECTORY web/ DESTINATION share/remote-assist/web)
