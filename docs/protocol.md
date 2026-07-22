# 通信协议

## HTTP

GET / 返回 web/index.html;GET /app.js、/app.css 返回静态资源。

## WebSocket /ws

### 鉴权

连接建立后,浏览器第一帧必须是:

    {"t":"auth","token":"<明文密码>"}

agent 按 config.json 的 password_iterations 使用 PBKDF2-SHA256 校验 token；缺少该字段的历史配置兼容 SHA-256(salt + token)。失败则发送 {"t":"auth","ok":false,"reason":"bad token"} 并断开;成功发 {"t":"auth","ok":true} 并进入流模式。

### 下行(agent -> 浏览器)

1. 进入流模式先发一帧配置:

    {"t":"cfg","codec":"jpeg","w":1920,"h":1080,"fps":30,"stream_id":1}

   w/h 是当前推流尺寸；大桌面会在服务端按比例缩放以降低延迟。

2. 之后每个 JPEG 二进制消息前都会有帧元数据：

       {"t":"frame","id":42,"stream_id":1}

   浏览器绘制该帧（或因配置版本过期而丢弃）后，必须回传
   `{"t":"ack","id":42}`。服务端在收到确认前只保留一张待发送的最新帧，
   防止慢网络或浏览器解码积压导致画面延迟持续增长；一秒未确认会自动超时继续。

### 上行(浏览器 -> agent)

所有文本 JSON:

    {"t":"key","sc":28,"down":true}          // sc=Windows Set-1 scancode
    {"t":"mouse","x":0.5,"y":0.5,"btn":"left","down":true}
    {"t":"move","x":0.5,"y":0.5}             // 绝对坐标归一化
    {"t":"wheel","delta":-120}
    {"t":"ack","id":42}                       // 仅控制端绘制确认使用

坐标 0..1 相对于当前画面归一化；选择单显示器时，agent 会加上该显示器在虚拟桌面中的偏移后再注入。

## 错误码

- {"t":"auth","ok":false,"reason":"need auth first"}:第一帧不是文本/非 auth。
- {"t":"auth","ok":false,"reason":"bad token"}:密码不匹配。
- {"t":"error","msg":"..."}:运行期错误。
