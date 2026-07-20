# 通信协议

## HTTP

GET / 返回 web/index.html;GET /app.js、/app.css 返回静态资源。

## WebSocket /ws

### 鉴权

连接建立后,浏览器必须在 3 秒内发第一帧:

    {"t":"auth","token":"<明文密码>"}

agent 校验 SHA-256(salt + token) == password_hash,失败则发送 {"t":"auth","ok":false,"reason":"bad token"} 并断开;成功发 {"t":"auth","ok":true} 并进入流模式。

### 下行(agent -> 浏览器)

1. 进入流模式先发一帧配置:

    {"t":"cfg","codec":"avc3.XXXXXX","w":1920,"h":1080,"fps":30}

   codec 由 agent 从首个 SPS 解析(profile/constraint/level 拼成 avc3.<hex><hex><hex>),使用 in-band SPS/PPS,浏览器无需 description。

2. 之后二进制帧为 H.264 Annex-B NAL;IDR 帧前置 SPS/PPS/IDR。

### 上行(浏览器 -> agent)

所有文本 JSON:

    {"t":"key","sc":28,"down":true}          // sc=Windows Set-1 scancode
    {"t":"mouse","x":0.5,"y":0.5,"btn":"left","down":true}
    {"t":"move","x":0.5,"y":0.5}             // 绝对坐标归一化
    {"t":"wheel","delta":-120}

坐标 0..1 归一化,agent 端乘以桌面尺寸还原为绝对像素。

## 错误码

- {"t":"auth","ok":false,"reason":"need auth first"}:第一帧不是文本/非 auth。
- {"t":"auth","ok":false,"reason":"bad token"}:密码不匹配。
- {"t":"error","msg":"..."}:运行期错误。

