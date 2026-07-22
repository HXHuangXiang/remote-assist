# 通信协议

## HTTP

GET / 返回 web/index.html;GET /app.js、/app.css 返回静态资源。

## WebSocket /ws

### 鉴权

连接建立后,浏览器第一帧必须是:

    {"t":"auth","token":"<明文密码>"}

agent 按 config.json 的 password_iterations 使用 PBKDF2-SHA256 校验 token；缺少该字段的历史配置兼容 SHA-256(salt + token)。失败则发送 {"t":"auth","ok":false,"reason":"bad token"} 并断开;成功发 {"t":"auth","ok":true} 并进入流模式。

认证失败会按来源 IP 采用 1、2、4…32 秒退避；单个来源的错误密码不会让其他局域网控制端进入同一退避期。服务同一时刻只处理一个未鉴权握手，避免空连接耗尽工作线程。

### 下行(agent -> 浏览器)

1. 进入流模式先发一帧配置:

    {"t":"cfg","codec":"avc1.42E01E","annexb":true,"w":1920,"h":1080,"fps":30,"stream_id":1}

   H.264 使用 Annex-B access unit 并由浏览器 WebCodecs 解码；`codec` 会按实际 SPS
   的 profile/compatibility/level 更新。每次新流、切屏或浏览器请求恢复时，首个 H.264
   帧必须是包含 SPS/PPS 与 IDR 的独立可解码关键帧。若当前机器无可用 H.264 MFT，codec
   会改为 `jpeg` 并由浏览器图片解码。w/h 是当前推流尺寸；大桌面会在服务端按比例缩放以降低延迟。

2. 之后每个 H.264/JPEG 二进制消息前都会有帧元数据：

       {"t":"frame","id":42,"stream_id":1,"key":true,"ts":33333}

   浏览器绘制该帧（或因配置版本过期而丢弃）后，必须回传
   `{"t":"ack","id":42}`。服务端最多允许两张已发送但尚未确认的帧，并只保留
   一张待发送帧，使网络写入、浏览器解码与下一帧传输能够重叠；窗口仍严格有界，
   防止慢网络或浏览器解码积压导致画面延迟持续增长。一秒未确认会自动释放一个窗口。
   JPEG 帧可以直接以最新帧覆盖旧帧；H.264 不会跳过中间 delta 帧，若待发送
   delta 已被新帧挤压，服务端会清空该帧并要求编码器输出新的 IDR，再从含
   SPS/PPS 的独立关键帧恢复解码。

3. 指针位置变化不触发整帧视频编码，而是发送独立文本消息：

       {"t":"cursor","visible":true,"x":0.5,"y":0.5}

   `x`、`y` 相对于当前推流画面归一化。浏览器在视频 canvas 上叠加通用箭头；指针
   离开选中的单显示器或不可见时发送 `{"t":"cursor","visible":false}`。当前 MVP
   传输位置和可见性，不传输 Windows 原生指针形状。

### 上行(浏览器 -> agent)

所有文本 JSON:

    {"t":"key","sc":28,"down":true}          // sc=Windows Set-1 scancode
    {"t":"mouse","x":0.5,"y":0.5,"btn":"left","down":true}
    {"t":"move","x":0.5,"y":0.5}             // 绝对坐标归一化
    {"t":"wheel","delta":-120}
    {"t":"ack","id":42}                       // 仅控制端绘制确认使用
    {"t":"keyframe"}                             // WebCodecs 恢复后请求下一张 IDR
    {"t":"client_stats","drawn":20,"dropped":1,"draw_ms_total":80,
     "draw_ms_samples":20,"decode_errors":0,"max_decode_queue":1,
     "max_ws_buffered":512}                       // 浏览器每 5 秒诊断上报

坐标 0..1 相对于当前画面归一化；选择单显示器时，agent 会加上该显示器在虚拟桌面中的偏移后再注入。

`client_stats` 不参与控制逻辑，仅用于写入 agent 的 `stream metrics`：它能把服务端
ACK 延迟进一步拆分为浏览器接收至实际绘制的耗时、解码队列峰值和页面上行发送缓冲峰值。
鼠标移动和滚轮在浏览器上行缓冲超过 16 KiB 时会合并为最新状态；键盘、鼠标按下/抬起和
ACK 不会被该背压策略丢弃。

## 错误码

- {"t":"auth","ok":false,"reason":"need auth first"}:第一帧不是文本/非 auth。
- {"t":"auth","ok":false,"reason":"bad token"}:密码不匹配。
- {"t":"error","msg":"..."}:运行期错误。
