#pragma once

// 确保不引入 OpenSSL(我们的 WebSocket 走 HTTP 明文,#ifdef 会把 =0 也视为已定义)。
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace remote_assist {

// 推流端累计计数。所有字段均可由采集线程无锁读取，用于写入 agent 诊断日志。
struct BroadcasterStats {
    uint64_t queuedFrames = 0;
    uint64_t replacedFrames = 0;
    uint64_t sentFrames = 0;
    uint64_t sentBytes = 0;
    uint64_t acknowledgedFrames = 0;
    // 从发送线程完成写入到浏览器绘制确认的累计延迟，用于 Agent 在不堆积 H.264
    // 参考帧的前提下自适应降低采集帧率。
    uint64_t ackLatencyUs = 0;
    uint64_t ackLatencySamples = 0;
    uint64_t ackTimeouts = 0;
    uint64_t sendFailures = 0;
    // H.264 的增量帧不能跳帧。此计数表示发送队列检测到将要覆盖增量帧，
    // 因而主动丢弃并要求 Agent 从下一张 IDR 重新开始的次数。
    uint64_t h264Resyncs = 0;
};

// 发送队列的入队结果。JPEG 可以安全地以最新帧覆盖旧帧；H.264 一旦覆盖
// 未发送的增量帧，后续 P/B 帧就可能失去参考帧，必须等下一张 IDR。
enum class FrameQueueResult {
    kQueued,
    kReplaced,
    kH264ResyncRequired,
    kStopped,
};

// 维护当前已通过鉴权的 WebSocket 连接,供 agent 编码线程广播 JPEG 帧或文本 JSON。
class WsBroadcaster {
public:
    WsBroadcaster();
    ~WsBroadcaster();

    // 仅允许一个控制端连接。返回 false 表示已有连接。
    bool Add(httplib::ws::WebSocket* ws);
    // 仅当 ws 仍是当前控制端时返回 true。旧连接在新控制端接管后退出时不能再
    // 触发输入释放，否则会错误中断新控制端正在按下的键鼠状态。
    bool Remove(httplib::ws::WebSocket* ws);
    // 取得编码器生成的帧所有权。JPEG 只保留最新一帧；H.264 不能跳过中间
    // 增量帧，若队列已满则清空待发送帧并要求调用方强制下一张 IDR。
    // streamId 与配置消息对应，用于浏览器丢弃过期图像。
    FrameQueueResult BroadcastBinary(std::vector<uint8_t> frame, uint64_t streamId,
                                     bool isKeyFrame, uint64_t timestampUs, bool h264);
    // H.264 的 delta 帧不能像 JPEG 一样以最新帧覆盖。编码前等待待发送槽位释放，
    // 可以直接跳过本轮采集输入而不产生缺失的参考帧，避免编码后再触发 IDR 重同步。
    // 返回 false 表示等待超时或 broadcaster 正在停止。
    bool WaitForH264FrameCredit(std::chrono::milliseconds timeout);
    // 文本下行与视频帧共用唯一发送线程，避免慢 socket 写操作阻塞采集线程。
    // replaceable=true 用于高频 cursor 位置，只保留最新一条；配置等关键消息
    // 使用默认值，按顺序可靠发送。
    void BroadcastText(std::string msg, bool replaceable = false);
    // 浏览器在帧真正绘制（或主动丢弃过期帧）后确认，服务端释放对应的发送窗口。
    void AcknowledgeFrame(uint64_t frameId);
    BroadcasterStats SnapshotStats() const;
    int Count();
    void Stop();

private:
    void SendLoop();

    // clientMu_ 在发送期间保持锁定，防止 WebSocket 回调返回后指针失效。
    std::mutex clientMu_;
    httplib::ws::WebSocket* client_ = nullptr;

    // 只保留尚未发送的最新一帧，慢客户端不会拖慢采集/编码线程。
    std::mutex frameMu_;
    std::condition_variable frameCv_;
    std::vector<uint8_t> pendingFrame_;
    uint64_t pendingStreamId_ = 0;
    uint64_t pendingTimestampUs_ = 0;
    bool pendingKeyFrame_ = true;
    // 配置等关键文本消息不能被高频 cursor 覆盖；cursor 则独立合并为最新状态。
    // 发送循环在视频与 cursor 间交替，既保持指针低延迟，也不能让鼠标移动饿死视频。
    std::deque<std::string> pendingText_;
    std::string pendingReplaceableText_;
    bool preferReplaceableText_ = false;
    uint64_t nextFrameId_ = 1;
    struct InFlightFrame {
        uint64_t id = 0;
        std::chrono::steady_clock::time_point sentAt{};
    };
    // 两帧窗口允许网络写入、WebCodecs 解码和下一帧传输重叠，避免每帧都完整
    // 等待一轮浏览器 ACK；仍是严格有界队列，慢客户端不会无限积压。
    std::deque<InFlightFrame> inFlightFrames_;
    bool stopping_ = false;
    std::thread senderThread_;

    std::atomic<uint64_t> queuedFrames_{0};
    std::atomic<uint64_t> replacedFrames_{0};
    std::atomic<uint64_t> sentFrames_{0};
    std::atomic<uint64_t> sentBytes_{0};
    std::atomic<uint64_t> acknowledgedFrames_{0};
    std::atomic<uint64_t> ackLatencyUs_{0};
    std::atomic<uint64_t> ackLatencySamples_{0};
    std::atomic<uint64_t> ackTimeouts_{0};
    std::atomic<uint64_t> sendFailures_{0};
    std::atomic<uint64_t> h264Resyncs_{0};
};

// HTTP + WebSocket 服务。HTTP 挂载 web/ 静态页面;WebSocket /ws 承载鉴权、配置下发与键鼠事件。
class HttpWsServer {
public:
    using OnMessage = std::function<void(const std::string& msg)>;
    using OnControllerDisconnected = std::function<void()>;
    using AuthVerifier = std::function<bool(const std::string& token)>;
    using CfgProvider = std::function<std::string()>;

    HttpWsServer();
    ~HttpWsServer();

    HttpWsServer(const HttpWsServer&) = delete;
    HttpWsServer& operator=(const HttpWsServer&) = delete;

    // 挂载控制端静态资源。目录不存在时返回 false，避免服务端口已监听但浏览器
    // 只能得到 404 的半可用状态。
    bool SetWebDir(const std::string& dir);
    void SetAuthVerifier(AuthVerifier v);
    void SetCfgProvider(CfgProvider p);
    void SetOnMessage(OnMessage cb);
    void SetOnControllerDisconnected(OnControllerDisconnected cb);
    WsBroadcaster& Broadcaster() { return broadcaster_; }

    // 成功返回时 httplib 已进入 accept 循环，可安全对外报告 Agent 就绪。
    bool Start(const std::string& host, int port);
    // 监听线程异常退出时变为 false；Agent 据此退出，由服务进程按既有策略重拉。
    bool IsRunning() const { return running_.load(); }
    void Stop();

private:
    class AuthAttemptGuard;
    bool TryBeginAuthAttempt();
    void EndAuthAttempt();
    bool CanAttemptAuth(const std::string& remoteAddr);
    void RecordAuthFailure(const std::string& remoteAddr);
    void ResetAuthFailures(const std::string& remoteAddr);

    httplib::Server svr_;
    WsBroadcaster broadcaster_;
    AuthVerifier authVerifier_;
    CfgProvider cfgProvider_;
    OnMessage onMessage_;
    OnControllerDisconnected onControllerDisconnected_;
    struct AuthThrottle {
        int failures = 0;
        std::chrono::steady_clock::time_point nextAttemptAt{};
        std::chrono::steady_clock::time_point lastSeenAt{};
    };
    std::mutex authMu_;
    // 仅允许一个客户端占用“首帧认证”读取槽位，避免多个空 WebSocket 将 httplib
    // 的小线程池耗尽；认证通过后立即释放该槽位，正常控制端仍由 broadcaster 限制。
    bool authAttemptInProgress_ = false;
    std::unordered_map<std::string, AuthThrottle> authThrottles_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace remote_assist
