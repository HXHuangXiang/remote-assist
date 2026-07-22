# RemoteAssist

Windows 局域网浏览器远控 MVP。被控端单 exe 以 LocalSystem 服务常驻,内置 HTTP + WebSocket 服务;局域网内任意现代浏览器(Chromium/Edge 113+)打开页面即可看到远端画面并通过键鼠事件回传控制,支持锁屏界面可见可操作。

## 设计要点

- 单 binary 三模式:--service(LocalSystem 服务)、--agent(由 service 用 winlogon token 启动到当前活跃桌面,做采集/编码/网络/注入)、--tray(用户会话托盘 UI)。
- 屏幕采集优先走 DXGI Desktop Duplication；多屏、锁屏与 DXGI 不可用场景回退 GDI BitBlt。
- 当前编码使用系统 WIC JPEG，浏览器端以图片解码绘制；服务端仅保留最新待发送帧，避免慢浏览器累积延迟。
- 鉴权:WebSocket 第一帧 {"t":"auth","token":"<密码>"};新配置以 PBKDF2-SHA256(210000 次)保存密码哈希，首次启动随机生成并在托盘展示一次。
- 安全立场:服务可见(托盘)、可退出,不做隐藏运行与静默持久化;不碰 UAC Secure Desktop,不做 Ctrl+Alt+Del 注入。

详见 docs/architecture.md 与 docs/protocol.md。

## 构建

前置:Visual Studio 2022、Windows 10 SDK、CMake 3.21+。

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

产物:build/Release/RemoteAssist.exe(单 exe,静态 CRT,< 8MB)。third_party 已含 cpp-httplib 与 nlohmann/json 的 header-only 版本,无需额外下载。

## 安装与卸载

管理员 CMD 执行 tools/install.bat(需与 RemoteAssist.exe 同目录),或手动:

    sc create remote-assist binPath= "\"<绝对路径>\RemoteAssist.exe\" --service" start= auto obj= LocalSystem
    sc description remote-assist "RemoteAssist 远控被控端"
    sc start remote-assist

卸载:

    sc stop remote-assist
    sc delete remote-assist

## 使用

1. 启动服务后,托盘图标会弹出首次生成的访问密码。
2. 在同一局域网任一浏览器打开 http://<被控机IP>:7980/。
3. 输入密码连接,看到画面后键鼠操作会注入到远端当前桌面(含锁屏)。

运行日志位于 exe 同级的 logs/：service.log 记录服务与子进程拉起，agent.log 记录采集、编码与网络，tray.log 与 setup.log 分别记录本地界面。

## 约束

MVP 支持单控制客户端；多显示器可选择单屏或全部屏幕。明文 HTTP+密码仅适用于受信任局域网。
不做:文件传输、剪贴板、NAT 穿透、TLS、UAC Secure Desktop 注入、Ctrl+Alt+Del 注入、多客户端并发。

## 备注

- 此项目复刻了附件 PoC 的核心链路:Windows Service(LocalSystem) -> 复制 winlogon.exe token -> CreateProcessAsUser 启动 agent -> OpenInputDesktop/SetThreadDesktop -> 采集与 SendInput。在此之上增加了 DXGI/GDI 采集、WIC JPEG 编码、cpp-httplib WebSocket 推流与浏览器控制端。
- 键盘走 scancode 路径(KEYEVENTF_SCANCODE),对 Winlogon/锁屏桌面更兼容。
- WIC JPEG 编码和 DXGI 采集均使用 Windows 系统组件；首次编译若个别 SDK 符号未定义，请确认 Windows 10 SDK 已安装。
