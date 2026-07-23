# 性能诊断与回归记录

本项目的性能优化应以 `logs/agent.log` 每 10 秒输出的一条 `stream metrics` 为依据。
不要只凭浏览器画面卡顿就盲目提高码率或 FPS：锁屏时的 GDI 采集、编码器、浏览器
解码和局域网传输都会产生相似现象，但对应的处理方向不同。

## 回归步骤

1. 使用 Chromium 或 Edge 打开控制端，连接后持续移动鼠标并输入文字至少 30 秒。
2. 分别记录普通桌面、`Win+L` 锁屏桌面、单显示器和“全部屏幕”模式的最后三条
   `stream metrics`。
3. 每次只改变一个条件（显示器选择、网络、分辨率或登录状态），避免把多个变化
   混在同一组指标中。
4. 记录浏览器端是否出现黑屏、画面落后、鼠标落后或按键反馈落后；这些现象与下表的
   `diagnosis` 一起判断。

## 字段与处理方向

| `diagnosis` / 字段 | 说明 | 优先处理方向 |
| --- | --- | --- |
| `capture_gdi`，`gdi_blt_avg_us` 高，`adaptive_gdi_capture_load_pct` 高 | 锁屏、跨显卡或旋转屏走 GDI，完整 BitBlt 已接近帧预算。`gdi_blt_avg_us` 统计全部 GDI 复制尝试，不只统计最终编码的帧 | 优先选择单显示器；保留浏览器指针预测；观察自动降 FPS/分辨率后是否恢复 |
| `capture_dxgi`，`capture_frame_avg_ms` 高 | Desktop Duplication 或 GPU 合成耗时过高 | 检查驱动、显示器热插拔和跨显卡拓扑；比较单屏与全部屏幕 |
| `gpu_nv12_submit_avg_us` 高 | GPU NV12 路径在 CPU 侧的资源分配、视频处理器视图创建或提交调用耗时偏高 | 先排除驱动问题；只有该值持续偏高且 `d3d_input_wrap_avg_us` 较低时，才评估有界 GPU surface 池 |
| `encode`，`encode_avg_ms` 或 `bgra_nv12_avg_us` 高 | CPU 色彩转换或 H.264 MFT 是瓶颈 | 确认日志中 `input=d3d11`；无 GPU 输入时降低输出分辨率或 FPS |
| `browser`，`client_decode_avg_ms`、`client_draw_avg_ms` 高或 `client_decode_queue_max > 2` | 控制端 WebCodecs 解码或 Canvas 绘制跟不上。前者高表示解码慢；仅后者高则更可能是 rAF/Canvas | 使用 Edge/Chromium，关闭浏览器的高负载标签页，检查硬件加速 |
| `network`，`ack_avg_ms` 高、`ack_timeout`、`h264_resync` 或 `send_fail` 非零 | 网络或浏览器端到端消费延迟高 | 检查 Wi-Fi/有线链路；确认自适应 FPS、码率和分辨率是否已降档 |
| `diagnosis=normal` 但体感卡 | 指标未覆盖的系统或输入问题 | 同时记录 `stream_fps`、`stream_cap` 与浏览器录屏，排查本地窗口合成、GPU 驱动或远端应用自身卡顿 |

配置窗口的“画质上限”用于限制 `stream_cap` 能恢复到的最高分辨率：自动和 1080p 从
1080p 档开始，720p/540p 则分别从对应档开始。它不是固定输出分辨率；采集、编码或
网络过载时仍会进一步降档，恢复后不会超过所选上限。锁屏 GDI 路径持续显示
`capture_gdi` 时，优先改为 720p 或 540p 后再比较 `gdi_blt_avg_us` 与
`adaptive_gdi_capture_load_pct`。

## 延迟窗口约束

H.264 的端到端视频窗口最多两帧：已发送未确认帧与待发送帧合计计数。窗口满时，
Agent 跳过本轮采集输入，不会编码一个随后无法安全发送的预测帧。单次信用等待可能
来自浏览器一次 rAF/GC 抖动，不会立即触发自适应降帧；同一秒内出现至少两次等待才
会视为持续背压。鼠标连续移动直接由浏览器原生指针呈现，Agent 通过独立 `cursor`
消息同步远端的可见性和标准样式；GDI 采集仍以交互 FPS 读取真实状态。首个移动和
后续受限频率的移动可立即唤醒采集，避免把完整 BitBlt 放大到浏览器 `mousemove` 的
刷新率。

## 合格基线

局域网普通桌面下，应长期看到 `diagnosis=normal`、`ack_timeout=0`、
`h264_resync=0`，且 `client_decode_queue_max` 不超过 2。锁屏场景允许出现 GDI 路径，
但鼠标移动不应导致 `gdi_blt_avg_us` 的采样频率升到浏览器刷新率；键盘、点击和滚轮
后的画面则应在交互 FPS 内更新。
