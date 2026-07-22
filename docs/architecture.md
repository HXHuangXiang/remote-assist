# 架构

## 进程模型(单 binary 三模式)

    RemoteAssist.exe --service  (LocalSystem, Session 0)
      |- ServiceHost: SCM 注册与生命周期
      |- ProcessLauncher
      |    |- 复制 winlogon.exe token -> CreateProcessAsUser(--agent, lpDesktop=winsta0\default)
      |    \- 复制 explorer.exe token -> CreateProcessAsUser(--tray, 用户桌面)
      \- 保留 agent/tray 进程句柄并监控存活，退出则按需拉起；停止服务时回收子进程

    RemoteAssist.exe --agent   (由 winlogon token 启动,绑定当前 input desktop)
      |- DesktopAccess: 每个工作线程独立 OpenInputDesktop + SetThreadDesktop，每秒检查桌面切换
      |- Capture: DXGI Desktop Duplication(普通桌面)/ GDI BitBlt(锁屏回退)
      |- EncoderMf: WIC JPEG 编码，输出单张 JPEG 帧
      |- HttpWsServer: cpp-httplib 托管 web/ + /ws
      \- Input: SendInput 注入键鼠(锁屏桌面用 scancode 路径)

    RemoteAssist.exe --tray    (用户会话)
      \- TrayApp: Shell_NotifyIcon,显示密码/关于/退出

## 桌面跟随

采集线程和输入线程各自持有当前 input desktop。每秒按桌面名检测变化，变化后重建与旧桌面相关的采集资源，覆盖 锁屏<->解锁、Winlogon<->Default 切换。MVP 不处理 UAC Secure Desktop(Winlogon 桌面已满足锁屏可见可操作)。

## 安全边界

允许:复制 winlogon token 启动 agent 到 Winlogon 桌面,采集与注入键鼠;LocalSystem 服务常驻,开机自启;托盘可见运行,本地用户可随时退出。
不做:UAC Secure Desktop、Ctrl+Alt+Del 注入(需 SAS/驱动)、隐藏运行、静默持久化、规避本地用户终止;日志不写密码、屏幕内容、剪贴板。

## 配置文件

exe 同目录的 config.json:port、password_hash、salt、password_iterations、bitrate、fps；日志位于 exe 同目录 logs/，按 service.log、agent.log、tray.log、setup.log 分开写入，避免多进程抢写同一日志。新配置使用 PBKDF2-SHA256(210000 次)保存密码哈希；缺少 password_iterations 的历史配置继续按 SHA-256(salt + token) 校验，避免升级后密码失效。
首次启动生成随机密码与 salt,计算哈希并写回;同时把明文密码写入 .initial-password,供 tray 进程读取展示后删除。
