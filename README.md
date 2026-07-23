# RemoteAssist

Windows 局域网浏览器远控 MVP。被控端单 exe 以 LocalSystem 服务常驻,内置 HTTP + WebSocket 服务;局域网内任意现代浏览器(Chromium/Edge 113+)打开页面即可看到远端画面并通过键鼠事件回传控制,支持锁屏界面可见可操作。

## 设计要点

- 单 binary 三模式:--service(LocalSystem 服务)、--agent(由 service 用 winlogon token 启动到当前活跃桌面,做采集/编码/网络/注入)、--tray(用户会话托盘 UI)。
- 屏幕采集优先走 DXGI Desktop Duplication；“全部屏幕”在同一显卡适配器时会 GPU
  合成各输出，跨显卡、旋转屏、锁屏与 DXGI 不可用场景回退 GDI BitBlt。
- 编码优先使用系统 Media Foundation H.264 MFT（硬件优先、软件 MFT 兜底），浏览器以 WebCodecs 解码；不可用或运行期失效时自动回退 WIC JPEG。小面积桌面变化会按 JPEG 图块合成到浏览器现有画面，大面积变化继续发送完整视频帧。
- 鉴权:WebSocket 第一帧 {"t":"auth","token":"<密码>"};新配置以 PBKDF2-SHA256(210000 次)保存密码哈希，首次启动随机生成并在托盘展示一次。
- 安全立场:服务可见(托盘)、可退出,不做隐藏运行与静默持久化;不碰 UAC Secure Desktop,不做 Ctrl+Alt+Del 注入。

详见 docs/architecture.md 与 docs/protocol.md。

## 构建

前置:Visual Studio 2022、Windows 10 SDK、CMake 3.21+。

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    cmake --install build --config Release --prefix dist

产物:build/Release/RemoteAssist.exe(单 exe,静态 CRT,< 8MB)。`cmake --install` 会生成
`dist/RemoteAssist.exe` 与同级 `dist/web/`；运行时生成的配置和日志也位于该目录。
third_party 已含 cpp-httplib 与 nlohmann/json 的 header-only 版本,无需额外下载。

## 安装与卸载

双击 `RemoteAssist.exe`，在 UAC 提示中选择“是”，随后在配置窗口设置密码并点击
“安装/更新并启动”。该按钮会在当前目录创建或更新服务，并自动启动；已有服务会更新
SCM 映像路径后重启，从而加载当前版本。无需使用命令行或安装脚本。

升级时，先在配置窗口点击“停止服务”，再替换同目录的 `RemoteAssist.exe` 和 `web/`，
重新双击新版 exe 后点击“安装/更新并启动”。`config.json`、`.config.lock` 与 `logs/`
会继续保留在当前目录。卸载服务也请在同一配置窗口中点击“卸载服务”。

## 使用

1. 启动服务后,托盘图标会弹出首次生成的访问密码。
2. 在同一局域网任一浏览器打开 http://<被控机IP>:7980/。
3. 输入密码连接,看到画面后键鼠操作会注入到远端当前桌面(含锁屏)。

图形安装会创建仅限 Windows“专用网络”的 TCP 入站规则；若公司策略阻止创建规则，安装
提示会明确说明，需由管理员按配置端口手动放行。

运行日志位于 exe 同级的 logs/：service.log 记录服务与子进程拉起，agent.log 记录采集、编码与网络，并在有控制端时每 10 秒输出一次推流统计；tray.log 与 setup.log 分别记录本地界面。画质在网页工具栏选择：自动、原始清晰度或固定 1080p/720p/540p/360p。网页还可直接输入帧率（1–60 FPS）和码率（0.1–6.25 MB/s，1 MB/s = 8 Mbps），并从常用预设中选择；这些数值固定当前控制端的帧率和码率，自动画质仍可调整分辨率。局部更新阈值可选 10%–90%，默认 50%；局部精度可选择低 CPU（64×64）、平衡（32×32，默认）或省流（16×16），细小且相距较远的变化会分别传输。所有网页推流偏好仅保存到当前浏览器，不写入被控端配置；断开后 Agent 恢复服务端默认的自适应参数。性能字段的判读与普通桌面/锁屏回归步骤见 [性能诊断](/docs/performance-diagnostics.md)。

## 本机验收清单

Windows CI 只能验证编译和打包；首次部署或升级后，请在被控机按以下顺序验收：

1. 双击 exe 并在 UAC 提示中选择“是”，设置密码后安装并启动服务；状态先显示“Agent 已监听”，浏览器收到首帧后会变为“画面管线已就绪”。
2. 浏览器打开 `http://127.0.0.1:<端口>/`，确认能显示桌面并完成鼠标、键盘输入。
3. 锁定 Windows 后继续连接，确认锁屏画面可见；解锁后应自动回到普通桌面。
4. 从托盘双击或选择“打开配置窗口”，确认同一会话只保留一个托盘图标。
5. 在配置窗口停止服务，确认服务状态变为停止；重新启动后 Agent 应再次就绪。

任一步失败时，先查看 exe 同级 `logs/service.log`、`logs/agent.log`、`logs/tray.log` 和 `logs/setup.log`，尤其关注 `http/ws server listening`、`capture`、`H.264`、`ack_timeout` 与 `send_fail` 字段。

## 约束

MVP 支持单控制客户端；多显示器可选择单屏或全部屏幕。明文 HTTP+密码仅适用于受信任局域网。
不做:文件传输、剪贴板、NAT 穿透、TLS、UAC Secure Desktop 注入、Ctrl+Alt+Del 注入、多客户端并发。

## 备注

- 此项目复刻了附件 PoC 的核心链路:Windows Service(LocalSystem) -> 复制 winlogon.exe token -> CreateProcessAsUser 启动 agent -> OpenInputDesktop/SetThreadDesktop -> 采集与 SendInput。在此之上增加了 DXGI/GDI 采集、WIC JPEG 编码、cpp-httplib WebSocket 推流与浏览器控制端。
- 键盘走 scancode 路径(KEYEVENTF_SCANCODE),对 Winlogon/锁屏桌面更兼容。
- WIC JPEG 编码和 DXGI 采集均使用 Windows 系统组件；首次编译若个别 SDK 符号未定义，请确认 Windows 10 SDK 已安装。
