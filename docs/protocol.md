# 通信协议

## HTTP

GET / 返回 web/index.html;GET /app.js、/app.css 返回静态资源。

## WebSocket /ws

### 鉴权

连接建立后,浏览器第一帧必须是:

    {"t":"auth","token":"<明文密码>"}

agent 校验 SHA-256(salt + token) == password_hash,失败则发送 {"t":"auth","ok":false,"reason":"bad token"} 并断开;成功发 {"t":"auth","ok":true} 并进入流模式。

### 下行(agent -> 浏览器)

1. 进入流模式先发一帧配置:

    {"t":"cfg","codec":"jpeg","w":1920,"h":1080,"fps":30}

   w/h 是当前推流尺寸；大桌面会在服务端按比例缩放以降低延迟。

2. 之后每个二进制消息是一张完整 JPEG 图像。浏览器只绘制最新收到的帧，主动丢弃积压旧帧。

### 上行(浏览器 -> agent)

所有文本 JSON:

    {"t":"key","sc":28,"down":true}          // sc=Windows Set-1 scancode
    {"t":"mouse","x":0.5,"y":0.5,"btn":"left","down":true}
    {"t":"move","x":0.5,"y":0.5}             // 绝对坐标归一化
    {"t":"wheel","delta":-120}

坐标 0..1 相对于当前画面归一化；选择单显示器时，agent 会加上该显示器在虚拟桌面中的偏移后再注入。

## 错误码

- {"t":"auth","ok":false,"reason":"need auth first"}:第一帧不是文本/非 auth。
- {"t":"auth","ok":false,"reason":"bad token"}:密码不匹配。
- {"t":"error","msg":"..."}:运行期错误。
