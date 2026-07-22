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
#include <vector>

namespace remote_assist {

// 推流端累计计数。所有字段均可由采集线程无锁读取，用于写入 agent 诊断日志。
struct BroadcasterStats {
    uint64_t queuedFrames = 0;
    uint64_t replacedFrames = 0;
    uint64_t sentFrames = 0;
    uint64_t sentBytes = 0;
    uint64_t acknowledgedFrames = 0;
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
    void Remove(httplib::ws::WebSocket* ws);
    // 取得编码器生成的帧所有权。JPEG 只保留最新一帧；H.264 不能跳过中间
    // 增量帧，若队列已满则清空待发送帧并要求调用方强制下一张 IDR。
    // streamId 与配置消息对应，用于浏览器丢弃过期图像。
    FrameQueueResult BroadcastBinary(std::vector<uint8_t> frame, uint64_t streamId,
                                     bool isKeyFrame, uint64_t timestampUs, bool h264);
    void BroadcastText(const std::string& msg);
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

    void SetWebDir(const std::string& dir);
    void SetAuthVerifier(AuthVerifier v);
    void SetCfgProvider(CfgProvider p);
    void SetOnMessage(OnMessage cb);
    void SetOnControllerDisconnected(OnControllerDisconnected cb);
    WsBroadcaster& Broadcaster() { return broadcaster_; }

    bool Start(const std::string& host, int port);
    void Stop();

private:
    bool CanAttemptAuth();
    void RecordAuthFailure();
    void ResetAuthFailures();

    httplib::Server svr_;
    WsBroadcaster broadcaster_;
    AuthVerifier authVerifier_;
    CfgProvider cfgProvider_;
    OnMessage onMessage_;
    OnControllerDisconnected onControllerDisconnected_;
    std::mutex authMu_;
    int authFailures_ = 0;
    std::chrono::steady_clock::time_point nextAuthAt_{};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace remote_assist
