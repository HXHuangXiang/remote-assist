# 架构

## 进程模型(单 binary 三模式)

    RemoteAssist.exe --service  (LocalSystem, Session 0)
      |- ServiceHost: SCM 注册与生命周期
      |- ProcessLauncher
      |    |- 复制 winlogon.exe token -> CreateProcessAsUser(--agent, lpDesktop=winsta0\default)
      |    \- 复制 explorer.exe token -> CreateProcessAsUser(--tray, 用户桌面)
      \- 监控 agent/tray 存活,退出则按需拉起

    RemoteAssist.exe --agent   (由 winlogon token 启动,绑定当前 input desktop)
      |- DesktopAccess: OpenInputDesktop + SetThreadDesktop,周期重绑跟随锁屏<->解锁
      |- Capture: DXGI Desktop Duplication(普通桌面)/ GDI BitBlt(锁屏回退)
      |- EncoderMf: Media Foundation H.264,输出 Annex-B NAL
      |- HttpWsServer: cpp-httplib 托管 web/ + /ws
      \- Input: SendInput 注入键鼠(锁屏桌面用 scancode 路径)

    RemoteAssist.exe --tray    (用户会话)
      \- TrayApp: Shell_NotifyIcon,显示密码/关于/退出

## 桌面跟随

agent 周期(约每帧)OpenInputDesktop 比较句柄,变化则 SetThreadDesktop 重绑,覆盖 锁屏<->解锁、Winlogon<->Default 切换。MVP 不处理 UAC Secure Desktop(Winlogon 桌面已满足锁屏可见可操作)。

## 安全边界

允许:复制 winlogon token 启动 agent 到 Winlogon 桌面,采集与注入键鼠;LocalSystem 服务常驻,开机自启;托盘可见运行,本地用户可随时退出。
不做:UAC Secure Desktop、Ctrl+Alt+Del 注入(需 SAS/驱动)、隐藏运行、静默持久化、规避本地用户终止;日志不写密码、屏幕内容、剪贴板。

## 配置文件

%ProgramData%\RemoteAssist\config.json:port、password_hash(SHA-256 hex)、salt、bitrate、fps。
首次启动生成随机密码与 salt,计算哈希并写回;同时把明文密码写入 .initial-password,供 tray 进程读取展示后删除。

