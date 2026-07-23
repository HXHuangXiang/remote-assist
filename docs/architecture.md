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
      |- Capture: DXGI Desktop Duplication（单屏；同 adapter 多屏 GPU 合成）/
      |           GDI BitBlt（锁屏、跨 adapter/旋转多屏回退、静态首帧兜底；交互 5~30 FPS 自适应、静止 2 FPS）
      |- EncoderMf: Media Foundation H.264 MFT（硬件优先）编码；不可用时回退 WIC JPEG
      |- HttpWsServer: cpp-httplib 托管 web/ + /ws
      \- Input: SendInput 注入键鼠(锁屏桌面用 scancode 路径)

    RemoteAssist.exe --tray    (用户会话)
      \- TrayApp: Shell_NotifyIcon,显示密码/关于/退出

## 桌面跟随

采集线程和输入线程各自持有当前 input desktop。每秒比较当前与已绑定 desktop 的底层 user object，变化后重建与旧桌面相关的采集资源，覆盖 锁屏<->解锁、Winlogon<->Default 切换，以及注销再登录产生的同名 Default 实例替换。新控制端或解码恢复请求完整首帧时，若 DXGI Desktop Duplication 仅返回超时/指针更新，则会一次性通过同一 input desktop 的 GDI 获取基线画面，避免静态桌面持续黑屏；后续继续优先使用 DXGI。MVP 不处理 UAC Secure Desktop(Winlogon 桌面已满足锁屏可见可操作)。

同一轮检查也会刷新显示器拓扑。热插拔、分辨率变化或被选显示器消失时，采集资源会重建；
若原设备名仍存在则保留选择，否则回退“全部屏幕”。WebSocket 线程通过显示器快照生成 cfg，
不会与采集线程重枚举共享可变容器。

## 安全边界

允许:复制 winlogon token 启动 agent 到 Winlogon 桌面,采集与注入键鼠;LocalSystem 服务常驻,开机自启;托盘可见运行,本地用户可随时退出。
不做:UAC Secure Desktop、Ctrl+Alt+Del 注入(需 SAS/驱动)、隐藏运行、静默持久化、规避本地用户终止;日志不写密码、屏幕内容、剪贴板。

图形配置窗口是唯一的服务安装入口：无参数双击 exe 会先经 UAC 进入管理员配置窗口，随后
在当前 exe 目录创建配置、日志并创建或更新服务。网页资源、配置与日志不会自动迁移；
升级时由用户在配置窗口停止服务后替换 exe 和 `web/`，再通过“安装/更新并启动”应用新版。
--service、--agent 和由服务拉起的 --tray 不会触发该 UAC 路径。

服务创建的 Agent stop/listener-ready/frame-ready 全局事件显式限制为 LocalSystem 与管理员
可修改，交互式用户仅能读取状态。listener-ready 仅表示 HTTP/WebSocket 已监听；frame-ready
必须在当前视频流实际采集、编码并入队首帧后才置位；桌面、显示器、输出分辨率、编码器参数
或浏览器请求恢复发生变化时会先复位，避免黑屏时误报上一轮画面可用。这不会改变可见托盘与
正常服务停止入口，但能避免普通进程伪造就绪或直接干扰 Agent 生命周期。

## 配置文件

H.264 的端到端视频窗口以“已发送未确认 + 待发送”合计计算，最多两帧；窗口满时
跳过本轮采集输入而不生成会丢失参考关系的 delta，避免额外积压一帧陈旧画面。

GDI 路径中，键盘、按键鼠标和滚轮会立即唤醒采集；连续鼠标移动则由浏览器本地
预测指针，并在后续 `cursor` 消息中以实际桌面位置校正。首个移动和后续受限频率的
移动可唤醒采集，避免每个 mousemove 都触发一次完整 BitBlt。

exe 同目录的 config.json:port、password_hash、salt、password_iterations、bitrate、fps、quality_cap；日志位于 exe 同目录 logs/，按 service.log、agent.log、tray.log、setup.log 分开写入，避免多进程抢写同一日志。Tray 由普通用户令牌运行时，日志通过仅授权当前活动用户写入的本地命名管道转交 LocalSystem 服务落盘，避免 Tray 和服务直接争用同一个日志文件。agent 在有控制端时每 10 秒记录捕获、编码、发送和帧确认统计，用于定位卡顿；`diagnosis` 会明确输出 capture_gdi/capture_dxgi、encode、browser、network 或 normal，可直接判断下一步应优化的流水线阶段。CPU 回退时同一条指标还会给出 `capture_cpu_copy_scale_avg_us`、`gdi_blt_avg_us`、`bgra_nv12_avg_us` 和 `mf_input_prepare_avg_us`，分别定位缩放、锁屏 BitBlt、颜色转换与 Media Foundation 输入对象创建。GDI 直接借用最终 DIB 到本轮同步编码结束，不再额外复制整张 BGRA 帧。日志中的 `h264_ack_window_ms` 表示按浏览器实际绘制确认动态收敛的 350~1000ms H.264 确认窗口，避免偶发浏览器抖动反复触发 IDR。fps 与 bitrate 是配置上限，Agent 会根据浏览器 ACK 端到端延迟、发送失败、H.264 发送信用等待、重同步和真实编码时间预算，在 5 FPS 到该上限间自适应降低帧率，并在 MFT 支持时同步下调码率。quality_cap 可取自动(0)、1080、720、540；自动从 1080p 档开始，手动档位限制自适应恢复时的最高分辨率，过载时仍可继续下调。GDI 回退会直接把选中的源区域 StretchBlt 到最终推流尺寸 DIB，不再创建完整虚拟桌面 DIB 后做 CPU 裁剪和缩放；单帧超过可用帧预算时立即降 FPS，持续过载时按 1080p、900p、720p、540p、360p 的输出上限降低分辨率。DXGI 的桌面变化等待不会被误判为采集压力。远端键鼠操作后的 1.2 秒内 GDI 从 15 FPS 起步，并仅在 BitBlt 预算充足时逐步提高到最高 30 FPS；过载时会降至最低 5 FPS，静止时降至 2 FPS。有效远端输入会立即唤醒采集线程，因此不会等待完整的空闲帧周期。H.264 在编码前确认待发送槽位可用，槽位满时跳过采集输入而不生成会丢失参考关系的 delta 帧。网络、编码或 GDI 采集压力恢复后，输出质量会保守提升，避免慢链路或慢采集不断堆积增量帧。H.264/JPEG 的帧时间戳使用单调时钟，关键帧按真实经过时间而非初始化 FPS 的帧计数触发，因此降帧后浏览器时间基与关键帧周期仍然正确。新配置使用 PBKDF2-SHA256(210000 次)保存密码哈希；缺少 password_iterations 的历史配置继续按 SHA-256(salt + token) 校验，避免升级后密码失效。
历史 SHA-256 配置在首次正确认证后会使用新 salt 原子迁移为 PBKDF2-SHA256；迁移写入失败不会拒绝该次已验证连接，后续认证仍会继续尝试。密码配置窗口保存后若旧 Agent 恰好进行认证，迁移会复核磁盘中的密码存储，避免覆盖用户刚保存的新密码。
首次启动生成随机密码与 salt，计算哈希并写回。双击配置窗口首次生成密码时会立即提示用户保存；服务在无配置窗口的首启场景则创建随机命名、30 秒后自动擦除的只读共享内存，把密码交给当前会话 Tray 展示一次。共享内存只授权给 LocalSystem、管理员与目标 Tray 用户 SID，且 Tray 启动固定使用同一会话；日志不会记录通道名称。密码不会写入 .initial-password 或其他明文文件；遇到旧版本遗留的 .initial-password 会在配置加载时尽力删除。已存在的 config.json 若校验失败，程序不会静默覆盖或随机重置密码；Agent 会等待配置窗口设置新密码后显式保存完成修复，原文件保留以便排查。

采集失败（例如桌面切换中、驱动重置或锁屏桌面暂时不可读）时，CaptureLoop 会先重建一次资源，随后以 250ms 到 5s 的有界指数退避重试，避免黑屏状态按目标 FPS 持续创建 DXGI/GDI 对象；成功采集或确认静态画面后立即恢复正常节奏。
配置窗口和浏览器口令统一按 UTF-8 字节参与 PBKDF2；图形界面限制密码 UTF-8 编码后不超过
3072 字节，以适配 JSON 转义后的 WebSocket 首帧 8 KiB 上限。
