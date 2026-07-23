# 通信协议

## HTTP

GET / 返回 web/index.html;GET /app.js、/app.css 返回静态资源。

## WebSocket /ws

### 鉴权

连接建立后,浏览器第一帧必须是:

    {"t":"auth","token":"<明文密码>"}

agent 按 config.json 的 password_iterations 使用 PBKDF2-SHA256 校验 token；缺少该字段的历史配置兼容 SHA-256(salt + token)。历史口令首次校验成功后会生成新 salt 并原子升级为 PBKDF2；升级写入失败不影响该次已成功认证，后续认证会重试。token 必须是非空且 UTF-8 不超过 3072 字节；失败则发送 {"t":"auth","ok":false,"reason":"bad token"} 并断开;成功发 {"t":"auth","ok":true} 并进入流模式。

认证失败会按来源 IP 采用 1、2、4…32 秒退避；单个来源的错误密码不会让其他局域网控制端进入同一退避期。服务最多同时处理三个未鉴权握手，且同一来源同时只能占用一个；首帧必须在 2 秒内到达，超时和非文本首帧同样进入该来源退避。该时限按应用消息计算，单纯 Ping/Pong 不会延长它，避免空连接耗尽工作线程。认证通过后恢复既有的 5 秒底层读取时限与 2 秒 WebSocket heartbeat，正常浏览器的 Pong 可维持空闲会话，失联页面会被回收。

### 下行(agent -> 浏览器)

1. 进入流模式先发一帧配置:

    {"t":"cfg","codec":"avc1.42E01E","annexb":true,"w":1920,"h":1080,"fps":30,"stream_id":1}

   H.264 使用 Annex-B access unit 并由浏览器 WebCodecs 解码；帧 `ts` 由服务端单调
   时钟生成，随实际采集/编码节奏递增，不受自适应 FPS 下调前的初始化帧率影响；`codec` 会按实际 SPS
   的 profile/compatibility/level 更新。每次新流、切屏或浏览器请求恢复时，首个 H.264
   帧必须是包含 SPS/PPS 与 IDR 的独立可解码关键帧。若当前机器无可用 H.264 MFT，codec
   会改为 `jpeg` 并由浏览器图片解码。w/h 是当前推流尺寸；大桌面会在服务端按比例缩放以降低延迟。

2. 之后每个 H.264/JPEG 二进制消息前都会有帧元数据：

       {"t":"frame","id":42,"stream_id":1,"key":true,"ts":33333}

   浏览器绘制该帧（或因配置版本过期而丢弃）后，必须回传
   `{"t":"ack","id":42}`。服务端的端到端视频窗口最多保留两帧（已发送未确认
   与待发送合计），使网络写入、浏览器解码与下一帧传输能够重叠，同时不额外积压
   一张过时画面；窗口仍严格有界，防止慢网络或浏览器解码导致画面延迟持续增长。
   JPEG 300ms 未确认会释放一个
   窗口；H.264 的确认窗口首帧为 600ms，随后按已确认帧的实际绘制时延自适应收敛到
   350~1000ms。H.264 超时会清空未发送预测帧并请求下一张带 SPS/PPS 的 IDR，且会
   临时放宽下一次确认窗口，优先避免慢一拍的浏览器反复打断 GOP。
   JPEG 帧可以直接以最新帧覆盖旧帧；H.264 会在编码前等待端到端窗口空位，窗口满
   时跳过本轮采集输入，避免产生后续会缺失参考帧的 delta。极端竞态下若待发送
   delta 仍被新帧挤压，服务端才清空该帧并要求编码器输出新的 IDR，再从含 SPS/PPS
   的独立关键帧恢复解码。

3. 指针位置变化不触发整帧视频编码，而是发送独立文本消息：

       {"t":"cursor","visible":true,"x":0.5,"y":0.5}

   `x`、`y` 相对于当前推流画面归一化。浏览器在视频 canvas 上叠加通用箭头；指针
   离开选中的单显示器或不可见时发送 `{"t":"cursor","visible":false}`。当前 MVP
   传输位置和可见性，不传输 Windows 原生指针形状。控制端仅重绘指针附近的小区域，
   不会因高频 cursor 消息清空整张视频覆盖画布。

### 上行(浏览器 -> agent)

所有文本 JSON:

    {"t":"key","sc":28,"down":true}          // sc=Windows Set-1 scancode
    {"t":"mouse","x":0.5,"y":0.5,"btn":"left","down":true}
    {"t":"move","x":0.5,"y":0.5}             // 绝对坐标归一化
    {"t":"wheel","delta":-120}
    {"t":"ack","id":42}                       // 仅控制端绘制确认使用
    {"t":"keyframe"}                             // WebCodecs 恢复后请求下一张 IDR
    {"t":"client_stats","drawn":20,"dropped":1,"draw_ms_total":80,
     "draw_ms_samples":20,"decode_ms_total":35,"decode_ms_samples":20,
     "decode_errors":0,"max_decode_queue":1,
     "max_ws_buffered":512}                       // 浏览器每 5 秒诊断上报

坐标 0..1 相对于当前画面归一化；选择单显示器时，agent 会加上该显示器在虚拟桌面中的偏移后再注入。

`client_stats` 不参与控制逻辑，仅用于写入 agent 的 `stream metrics`：它能把服务端
ACK 延迟进一步拆分为浏览器接收至 WebCodecs 输出、接收至实际绘制的耗时、解码队列峰值
和页面上行发送缓冲峰值。`decode_ms_*` 为可选字段，旧版缓存页面未上报时 Agent 按零处理。
鼠标移动和滚轮在浏览器上行缓冲超过 16 KiB 时会合并为最新状态；键盘、鼠标按下/抬起和
ACK 不会被该背压策略丢弃。鼠标移动会先在浏览器预测远端指针位置；被控端后续的 `cursor`
消息仍会以真实桌面状态校正它，因此 GDI 回退无需为每个 mousemove 触发一次完整屏幕复制。

键盘仅在远端画布获得焦点时下发；画布获得焦点后会显示绿色边框。点击密码框、显示器选择框
或其他本地控件会立即释放远端已按下的键，避免本地输入误控远端或留下卡键。

## 错误码

- {"t":"auth","ok":false,"reason":"need auth first"}:第一帧不是文本/非 auth。
- {"t":"auth","ok":false,"reason":"bad token"}:密码不匹配。
- {"t":"error","code":"controller_cleanup","msg":"controller input cleanup in progress"}:旧控制端正在当前桌面释放遗留的 keyup/mouseup。控制端应等待连接关闭后短暂重试；内置页面最多自动重试 5 秒。
- {"t":"error","msg":"..."}:运行期错误。
