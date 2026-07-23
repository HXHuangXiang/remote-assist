#pragma once

#include "net/ControllerHandoff.h"
#include "net/H264FlowControl.h"

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
#include <unordered_set>
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
    // 当前控制端的 H.264 帧确认窗口。它基于已确认帧的真实绘制时延动态调整，
    // 避免慢一拍的浏览器被固定阈值反复强制 IDR。
    uint32_t h264AckTimeoutMs = 0;
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
    // 断连处理先进入输入清理态。此时旧 WebSocket 可能已经关闭，但只有所有
    // 遗留 keyup/mouseup 成功注入后，下一位控制端才能取得槽位。
    void BeginControllerInputCleanup();
    void CompleteControllerInputCleanup();
    // 用于在认证前快速拒绝已被占用或正等待输入释放的连接，避免无意义 PBKDF2。
    bool CanAcceptController();
    // 供 WebSocket 协议把“已有控制端”与“正在释放旧输入”区分开，后者可以由
    // 浏览器短暂重试，而不是要求用户再次点击连接。
    bool IsControllerInputCleanupPending();
    // 无控制端时 CaptureLoop 可在此等待。新控制端注册会立即唤醒，避免固定
    // 200ms 空闲轮询把首帧和重连后的输入反馈平白延后。
    bool WaitForActiveController(std::chrono::milliseconds timeout);
    // 取得编码器生成的帧所有权。JPEG 只保留最新一帧；H.264 不能跳过中间
    // 增量帧，若队列已满则清空待发送帧并要求调用方强制下一张 IDR。
    // streamId 与配置消息对应，用于浏览器丢弃过期图像。
    FrameQueueResult BroadcastBinary(std::vector<uint8_t> frame, uint64_t streamId,
                                     bool isKeyFrame, uint64_t timestampUs, bool h264);
    // H.264 的 delta 帧不能像 JPEG 一样以最新帧覆盖。编码前等待总视频窗口有
    // 空位（已发未确认 + 待发送合计最多两帧），可以直接跳过本轮采集输入而不
    // 产生缺失的参考帧，也避免多保留一张已过时画面而拉高交互延迟。
    // 返回 false 表示等待超时或 broadcaster 正在停止。
    bool WaitForH264FrameCredit(std::chrono::milliseconds timeout);
    // 文本下行与视频帧共用唯一发送线程，避免慢 socket 写操作阻塞采集线程。
    // replaceable=true 用于高频 cursor 位置，只保留最新一条；配置等关键消息
    // 使用默认值，按顺序可靠发送。
    void BroadcastText(std::string msg, bool replaceable = false);
    // 浏览器在帧真正绘制（或主动丢弃过期帧）后确认，服务端释放对应的发送窗口。
    void AcknowledgeFrame(uint64_t frameId);
    // H.264 帧确认超时后，发送端会清理旧窗口并请求编码线程从新的独立 IDR 恢复。
    // 返回 true 仅一次；调用方应在同一采集线程内请求关键帧，避免跨线程触碰 MFT。
    bool ConsumeH264ResyncRequest();
    BroadcasterStats SnapshotStats() const;
    int Count();
    void Stop();

private:
    void SendLoop();

    // clientMu_ 在发送期间保持锁定，防止 WebSocket 回调返回后指针失效。
    std::mutex clientMu_;
    std::condition_variable controllerCv_;
    httplib::ws::WebSocket* client_ = nullptr;
    // 与 client_ 在同一把锁下维护；它同时覆盖“handler 即将销毁”和“输入尚未
    // 释放”两个交接条件，避免两类线程交错时让新控制端提前进入。
    ControllerHandoff controllerHandoff_;

    // 只保留尚未发送的最新一帧，慢客户端不会拖慢采集/编码线程。
    std::mutex frameMu_;
    std::condition_variable frameCv_;
    std::vector<uint8_t> pendingFrame_;
    uint64_t pendingStreamId_ = 0;
    uint64_t pendingTimestampUs_ = 0;
    bool pendingKeyFrame_ = true;
    bool pendingH264_ = false;
    // 配置等关键文本消息不能被高频 cursor 覆盖；cursor 则独立合并为最新状态。
    // 发送循环在视频与 cursor 间交替，既保持指针低延迟，也不能让鼠标移动饿死视频。
    std::deque<std::string> pendingText_;
    std::string pendingReplaceableText_;
    bool preferReplaceableText_ = false;
    uint64_t nextFrameId_ = 1;
    struct InFlightFrame {
        uint64_t id = 0;
        // 发送线程选中帧后会先占用一个窗口，避免并发 ACK 与下一帧调度出现
        // “还未登记就收到 ACK”的竞态。只有二进制负载写入 socket 返回后才开始
        // ACK 超时计时；网络写入本身较慢不能被误判为浏览器解码/绘制缓慢。
        bool writeCompleted = false;
        // 极低延迟本机链路上，浏览器的 ACK 可能在发送线程重新取得 frameMu_
        // 前到达。保留该状态，随后由发送线程一次性确认并回收窗口。
        bool ackedDuringWrite = false;
        std::chrono::steady_clock::time_point sentAt{};
        std::chrono::steady_clock::time_point ackDeadline{};
        bool h264 = false;
    };
    // 两帧总窗口允许网络写入、WebCodecs 解码和下一帧传输重叠，避免每帧都完整
    // 等待一轮浏览器 ACK；待发送槽位也计入窗口，慢客户端不会额外积压一帧。
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
    std::atomic<uint32_t> h264AckTimeoutMs_{kH264AckTimeoutInitialMs};
    std::atomic<uint64_t> sendFailures_{0};
    std::atomic<uint64_t> h264Resyncs_{0};
    std::atomic<bool> h264ResyncRequested_{false};
    // 以下字段均由 frameMu_ 保护。每个 H.264 帧在实际 socket 写完时复制当前
    // 窗口，确保后续样本改变策略不会追溯性缩短已发帧的确认期限。
    uint32_t h264AckTimeoutWindowMs_ = kH264AckTimeoutInitialMs;
    bool h264AckTimeoutInitialized_ = false;
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
    class ActiveWebSocketGuard;
    bool TryBeginAuthAttempt(const std::string& remoteAddr);
    void EndAuthAttempt(const std::string& remoteAddr);
    // 认证读取与已认证控制会话共用同一个 WebSocket。服务停止前必须中断它：
    // httplib::Server::stop 只会关闭监听 socket，不能打断已进入 ws.read 的任务线程。
    // 这些方法由 connectionMu_ 串行化，确保 Stop 正在中断连接时 handler 不会销毁
    // 栈上的 WebSocket 对象。认证阶段允许少量不同来源并发，因此需追踪全部活动 socket。
    bool RegisterActiveSocket(httplib::ws::WebSocket* ws);
    void UnregisterActiveSocket(httplib::ws::WebSocket* ws);
    void AbortActiveSocketsForStop();
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
    // 空连接最多占用小线程池中的三个槽位，且同一来源同时只能有一条认证尝试。
    // 认证通过后立即释放该槽位，正常控制端仍由 broadcaster 限制。
    std::unordered_map<std::string, uint8_t> authAttemptsByAddress_;
    std::unordered_map<std::string, AuthThrottle> authThrottles_;
    std::mutex connectionMu_;
    // 仅在各 WebSocket handler 的栈帧有效期间保存。服务停止时持锁发送 Close，
    // handler 的 guard 则在同一把锁下注销，避免裸指针失效。
    std::unordered_set<httplib::ws::WebSocket*> activeSockets_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace remote_assist
