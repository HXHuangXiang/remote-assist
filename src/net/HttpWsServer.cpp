#include "net/HttpWsServer.h"

#include "common/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

namespace remote_assist {

namespace {

// 浏览器收到 WebSocket 二进制帧后还要经历图片解码和 canvas 绘制。仅靠 TCP
// 写成功无法说明用户已经看到新画面；超时兜底避免旧浏览器或异常连接永久停流。
constexpr auto kFrameAckTimeout = std::chrono::seconds(1);
constexpr size_t kMaxFramesInFlight = 2;
constexpr size_t kMaxPendingTextMessages = 8;
constexpr auto kAuthThrottleRetention = std::chrono::minutes(10);
constexpr size_t kMaxAuthThrottleEntries = 64;

// WebSocket handler 可能因为第三方回调、内存分配等异常提前离开。连接一旦已
// 注册到 broadcaster，就必须在所有退出路径移除，避免发送线程持有悬空指针。
class ControllerSessionGuard {
public:
    ControllerSessionGuard(WsBroadcaster& broadcaster, httplib::ws::WebSocket& ws,
                           std::function<void()> onDisconnected)
        : broadcaster_(broadcaster), ws_(ws), onDisconnected_(std::move(onDisconnected)) {}

    void Arm() { armed_ = true; }

    ~ControllerSessionGuard() {
        if (!armed_) {
            return;
        }
        // 保持 controller 槽位占用直到所有按下状态已释放。若先 Remove，再由新的
        // WebSocket 连接抢占槽位，旧连接的 ReleaseAll 会误抬起新控制端刚按下的键。
        // 关闭中的旧 socket 不再接收输入，但 Add 仍会因 client_ 非空而拒绝接管。
        try {
            if (onDisconnected_) {
                onDisconnected_();
            }
        } catch (...) {
            // 析构函数绝不能因业务回调异常终止 WebSocket 工作线程。
            log::Error("controller disconnect callback threw an exception");
        }
        if (broadcaster_.Remove(&ws_)) {
            log::Info("ws client disconnected");
        }
    }

private:
    WsBroadcaster& broadcaster_;
    httplib::ws::WebSocket& ws_;
    std::function<void()> onDisconnected_;
    bool armed_ = false;
};

}  // namespace

// 首帧认证的 scope guard。无论 ws.read、JSON 解析或回调以何种方式提前返回，
// 都会释放唯一的认证槽位，避免服务永久显示“server busy”。
class HttpWsServer::AuthAttemptGuard {
public:
    explicit AuthAttemptGuard(HttpWsServer& server) : server_(server) {}
    ~AuthAttemptGuard() {
        if (active_) {
            server_.EndAuthAttempt();
        }
    }

    void Complete() {
        if (active_) {
            server_.EndAuthAttempt();
            active_ = false;
        }
    }

private:
    HttpWsServer& server_;
    bool active_ = true;
};

// WebSocket 对象由 httplib handler 在栈上持有，不能把裸指针复制到 Stop 之后再
// 使用。guard 的析构与 CloseActiveSocketForStop 使用同一把锁，因此 Stop 持锁
// 发送 Close 帧时，handler 不会抢先返回并销毁对象。
class HttpWsServer::ActiveWebSocketGuard {
public:
    ActiveWebSocketGuard(HttpWsServer& server, httplib::ws::WebSocket& ws)
        : server_(server), ws_(ws), registered_(server_.RegisterActiveSocket(&ws_)) {}

    ~ActiveWebSocketGuard() {
        if (registered_) {
            server_.UnregisterActiveSocket(&ws_);
        }
    }

    bool Registered() const { return registered_; }

private:
    HttpWsServer& server_;
    httplib::ws::WebSocket& ws_;
    bool registered_ = false;
};

WsBroadcaster::WsBroadcaster()
    : senderThread_(&WsBroadcaster::SendLoop, this) {}

WsBroadcaster::~WsBroadcaster() {
    Stop();
}

bool WsBroadcaster::Add(httplib::ws::WebSocket* ws) {
    // 在暴露 client_ 前清掉上一位控制端遗留的队列。否则 CaptureLoop 可能刚看到
    // Count()==1 就入队首个 IDR，随后又被 Add 的清理步骤丢弃，导致新页面等不到画面。
    std::lock_guard<std::mutex> frameLock(frameMu_);
    std::lock_guard<std::mutex> clientLock(clientMu_);
    // 即使旧连接刚进入关闭态，也必须等其 handler 在 Remove 前完成全局按键释放，
    // 否则旧控制者遗留的 Ctrl/鼠标按下会带到新会话。
    if (client_) {
        return false;
    }
    client_ = ws;
    pendingFrame_.clear();
    pendingStreamId_ = 0;
    pendingTimestampUs_ = 0;
    pendingKeyFrame_ = true;
    pendingText_.clear();
    pendingReplaceableText_.clear();
    preferReplaceableText_ = false;
    inFlightFrames_.clear();
    frameCv_.notify_all();
    return true;
}

bool WsBroadcaster::Remove(httplib::ws::WebSocket* ws) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(clientMu_);
        if (client_ == ws) {
            client_ = nullptr;
            removed = true;
        }
    }
    if (removed) {
        std::lock_guard<std::mutex> lk(frameMu_);
        // 新控制端连接后由采集线程生成首帧；不要把断连前的旧图像带过去。
        pendingFrame_.clear();
        pendingStreamId_ = 0;
        pendingTimestampUs_ = 0;
        pendingKeyFrame_ = true;
        pendingText_.clear();
        pendingReplaceableText_.clear();
        preferReplaceableText_ = false;
        inFlightFrames_.clear();
        frameCv_.notify_all();
    }
    return removed;
}

FrameQueueResult WsBroadcaster::BroadcastBinary(std::vector<uint8_t> frame,
                                                uint64_t streamId, bool isKeyFrame,
                                                uint64_t timestampUs, bool h264) {
    bool replaced = false;
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        if (stopping_) {
            return FrameQueueResult::kStopped;
        }
        if (!pendingFrame_.empty()) {
            // JPEG 帧没有前后依赖，直接替换可把延迟保持在一帧以内。H.264
            // delta 帧则会引用被替换的上一帧，若仍把最新 delta 发给浏览器会
            // 触发解码失败/黑屏。此时清空待发送帧，并让 Agent 请求新的 IDR。
            if (h264 && !isKeyFrame) {
                pendingFrame_.clear();
                pendingStreamId_ = 0;
                pendingTimestampUs_ = 0;
                pendingKeyFrame_ = true;
                replacedFrames_.fetch_add(1, std::memory_order_relaxed);
                h264Resyncs_.fetch_add(1, std::memory_order_relaxed);
                return FrameQueueResult::kH264ResyncRequired;
            }
            replacedFrames_.fetch_add(1, std::memory_order_relaxed);
            replaced = true;
        }
        pendingFrame_ = std::move(frame);
        pendingStreamId_ = streamId;
        pendingKeyFrame_ = isKeyFrame;
        pendingTimestampUs_ = timestampUs;
        queuedFrames_.fetch_add(1, std::memory_order_relaxed);
    }
    frameCv_.notify_one();
    return replaced ? FrameQueueResult::kReplaced : FrameQueueResult::kQueued;
}

bool WsBroadcaster::WaitForH264FrameCredit(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(frameMu_);
    const bool ready = frameCv_.wait_for(lk, timeout, [this] {
        // 不要求 in-flight 小于窗口上限：允许有一张 H.264 delta 在待发送槽位中，
        // 这样 ACK、网络写和下一次采集能重叠；但绝不允许第二张 delta 覆盖它。
        return stopping_ || pendingFrame_.empty();
    });
    return ready && !stopping_;
}

void WsBroadcaster::BroadcastText(std::string msg, bool replaceable) {
    // 无控制端时沿用原来的 no-op 语义。桌面切换/显示器热插拔可能发生在空闲期，
    // 不能把 cfg 留到下一位控制端，也不应把它计为一次发送失败。
    if (msg.empty() || Count() == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        if (stopping_) {
            return;
        }
        if (replaceable) {
            pendingReplaceableText_ = std::move(msg);
        } else {
            // 配置消息以最新状态为准。连接刚切屏或恢复时，保留最末八条足够覆盖
            // 短暂发送阻塞，又避免异常频繁的拓扑变化无限占用内存。
            if (pendingText_.size() == kMaxPendingTextMessages) {
                pendingText_.pop_front();
            }
            pendingText_.push_back(std::move(msg));
        }
    }
    frameCv_.notify_one();
}

void WsBroadcaster::AcknowledgeFrame(uint64_t frameId) {
    if (frameId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lk(frameMu_);
    const auto it = std::find_if(inFlightFrames_.begin(), inFlightFrames_.end(),
        [frameId](const InFlightFrame& frame) { return frame.id == frameId; });
    if (it != inFlightFrames_.end()) {
        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - it->sentAt).count();
        inFlightFrames_.erase(it);
        acknowledgedFrames_.fetch_add(1, std::memory_order_relaxed);
        ackLatencyUs_.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, latency)),
                                std::memory_order_relaxed);
        ackLatencySamples_.fetch_add(1, std::memory_order_relaxed);
        // sender 和采集线程都可能在等同一个状态变化。只唤醒一个会导致采集线程
        // 抢到通知却因 pending 仍存在继续休眠，sender 则要等到 ACK 超时才会继续。
        frameCv_.notify_all();
    }
}

BroadcasterStats WsBroadcaster::SnapshotStats() const {
    BroadcasterStats stats;
    stats.queuedFrames = queuedFrames_.load(std::memory_order_relaxed);
    stats.replacedFrames = replacedFrames_.load(std::memory_order_relaxed);
    stats.sentFrames = sentFrames_.load(std::memory_order_relaxed);
    stats.sentBytes = sentBytes_.load(std::memory_order_relaxed);
    stats.acknowledgedFrames = acknowledgedFrames_.load(std::memory_order_relaxed);
    stats.ackLatencyUs = ackLatencyUs_.load(std::memory_order_relaxed);
    stats.ackLatencySamples = ackLatencySamples_.load(std::memory_order_relaxed);
    stats.ackTimeouts = ackTimeouts_.load(std::memory_order_relaxed);
    stats.sendFailures = sendFailures_.load(std::memory_order_relaxed);
    stats.h264Resyncs = h264Resyncs_.load(std::memory_order_relaxed);
    return stats;
}

int WsBroadcaster::Count() {
    std::lock_guard<std::mutex> lk(clientMu_);
    return (client_ && client_->is_open()) ? 1 : 0;
}

void WsBroadcaster::Stop() {
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        pendingFrame_.clear();
        pendingStreamId_ = 0;
        pendingTimestampUs_ = 0;
        pendingKeyFrame_ = true;
        pendingText_.clear();
        pendingReplaceableText_.clear();
        preferReplaceableText_ = false;
        inFlightFrames_.clear();
    }
    frameCv_.notify_all();
    if (senderThread_.joinable()) {
        senderThread_.join();
    }
}

void WsBroadcaster::SendLoop() {
    for (;;) {
        std::vector<uint8_t> frame;
        std::string text;
        uint64_t streamId = 0;
        uint64_t frameId = 0;
        uint64_t timestampUs = 0;
        bool isKeyFrame = true;
        bool sendingFrame = false;
        {
            std::unique_lock<std::mutex> lk(frameMu_);
            for (;;) {
                if (stopping_) {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                if (!inFlightFrames_.empty() &&
                    now >= inFlightFrames_.front().sentAt + kFrameAckTimeout) {
                    // 即使持续有 cursor 消息，也必须按时释放超时帧窗口；否则高频
                    // 指针移动会让 H.264 信用窗口永久卡在满载状态。
                    log::Warn("frame ack timed out, releasing one in-flight slot");
                    ackTimeouts_.fetch_add(1, std::memory_order_relaxed);
                    inFlightFrames_.pop_front();
                    continue;
                }
                if (!pendingText_.empty()) {
                    text = std::move(pendingText_.front());
                    pendingText_.pop_front();
                    break;
                }
                const bool canSendFrame = inFlightFrames_.size() < kMaxFramesInFlight &&
                    !pendingFrame_.empty();
                if (!pendingReplaceableText_.empty() &&
                    (!canSendFrame || preferReplaceableText_)) {
                    text = std::move(pendingReplaceableText_);
                    pendingReplaceableText_.clear();
                    preferReplaceableText_ = false;
                    break;
                }
                if (canSendFrame) {
                    frame.swap(pendingFrame_);
                    streamId = pendingStreamId_;
                    pendingStreamId_ = 0;
                    isKeyFrame = pendingKeyFrame_;
                    pendingKeyFrame_ = true;
                    timestampUs = pendingTimestampUs_;
                    pendingTimestampUs_ = 0;
                    frameId = nextFrameId_++;
                    inFlightFrames_.push_back({frameId, now});
                    preferReplaceableText_ = true;
                    sendingFrame = true;
                    break;
                }
                if (!pendingReplaceableText_.empty()) {
                    text = std::move(pendingReplaceableText_);
                    pendingReplaceableText_.clear();
                    preferReplaceableText_ = false;
                    break;
                }
                if (!inFlightFrames_.empty()) {
                    const auto deadline = inFlightFrames_.front().sentAt + kFrameAckTimeout;
                    if (frameCv_.wait_until(lk, deadline, [this] {
                        return stopping_ ||
                            !pendingText_.empty() || !pendingReplaceableText_.empty() ||
                            (inFlightFrames_.size() < kMaxFramesInFlight &&
                             !pendingFrame_.empty());
                        })) {
                        continue;
                    }
                    continue;
                }
                frameCv_.wait(lk, [this] {
                    return stopping_ ||
                        !pendingText_.empty() || !pendingReplaceableText_.empty() ||
                        (inFlightFrames_.size() < kMaxFramesInFlight && !pendingFrame_.empty());
                });
            }
        }
        if (sendingFrame) {
            // pendingFrame_ 已被取走，采集线程现在可安全编码下一张 delta。必须在
            // 释放 frameMu_ 后通知，避免它醒来后马上阻塞在同一把锁上。
            frameCv_.notify_all();
        }

        bool sent = false;
        {
            std::lock_guard<std::mutex> lk(clientMu_);
            if (client_ && client_->is_open()) {
                if (!sendingFrame) {
                    sent = client_->send(text);
                } else {
                    const std::string header = "{\"t\":\"frame\",\"id\":" +
                        std::to_string(frameId) + ",\"stream_id\":" + std::to_string(streamId) +
                        ",\"key\":" + (isKeyFrame ? "true" : "false") +
                        ",\"ts\":" + std::to_string(timestampUs) + "}";
                    sent = client_->send(header) &&
                        client_->send(reinterpret_cast<const char*>(frame.data()), frame.size());
                }
            }
        }
        if (sent && sendingFrame) {
            sentFrames_.fetch_add(1, std::memory_order_relaxed);
            sentBytes_.fetch_add(static_cast<uint64_t>(frame.size()), std::memory_order_relaxed);
        }
        if (!sent) {
            sendFailures_.fetch_add(1, std::memory_order_relaxed);
            if (sendingFrame) {
                std::lock_guard<std::mutex> frameLock(frameMu_);
                const auto it = std::find_if(inFlightFrames_.begin(), inFlightFrames_.end(),
                    [frameId](const InFlightFrame& inFlight) { return inFlight.id == frameId; });
                if (it != inFlightFrames_.end()) {
                    inFlightFrames_.erase(it);
                    frameCv_.notify_all();
                }
            }
        }
    }
}

HttpWsServer::HttpWsServer() = default;

HttpWsServer::~HttpWsServer() {
    Stop();
}

bool HttpWsServer::SetWebDir(const std::string& dir) {
    return svr_.set_mount_point("/", dir);
}

void HttpWsServer::SetAuthVerifier(AuthVerifier v) { authVerifier_ = std::move(v); }
void HttpWsServer::SetCfgProvider(CfgProvider p) { cfgProvider_ = std::move(p); }
void HttpWsServer::SetOnMessage(OnMessage cb) { onMessage_ = std::move(cb); }
void HttpWsServer::SetOnControllerDisconnected(OnControllerDisconnected cb) {
    onControllerDisconnected_ = std::move(cb);
}

bool HttpWsServer::TryBeginAuthAttempt() {
    std::lock_guard<std::mutex> lk(authMu_);
    if (stopping_.load(std::memory_order_acquire) || authAttemptInProgress_) {
        return false;
    }
    authAttemptInProgress_ = true;
    return true;
}

void HttpWsServer::EndAuthAttempt() {
    std::lock_guard<std::mutex> lk(authMu_);
    authAttemptInProgress_ = false;
}

bool HttpWsServer::RegisterActiveSocket(httplib::ws::WebSocket* ws) {
    if (!ws) {
        return false;
    }
    std::lock_guard<std::mutex> lk(connectionMu_);
    if (stopping_.load(std::memory_order_acquire) || activeSocket_) {
        return false;
    }
    activeSocket_ = ws;
    return true;
}

void HttpWsServer::UnregisterActiveSocket(httplib::ws::WebSocket* ws) {
    std::lock_guard<std::mutex> lk(connectionMu_);
    if (activeSocket_ == ws) {
        activeSocket_ = nullptr;
    }
}

void HttpWsServer::CloseActiveSocketForStop() {
    std::lock_guard<std::mutex> lk(connectionMu_);
    if (!activeSocket_ || !activeSocket_->is_open()) {
        return;
    }
    // 正常浏览器会立即回 Close 帧；不响应的对端仍受 WebSocket 读取超时保护。
    // 先设置 stopping_ 再关闭，确保认证刚成功的 handler 不会把已关闭连接重新
    // 注册为控制端。
    log::Info("requesting active WebSocket close for server shutdown");
    activeSocket_->close(httplib::ws::CloseStatus::GoingAway, "server stopping");
}

bool HttpWsServer::CanAttemptAuth(const std::string& remoteAddr) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(authMu_);
    for (auto it = authThrottles_.begin(); it != authThrottles_.end();) {
        if (now - it->second.lastSeenAt > kAuthThrottleRetention) {
            it = authThrottles_.erase(it);
        } else {
            ++it;
        }
    }
    const std::string key = remoteAddr.empty() ? "<unknown>" : remoteAddr;
    const auto found = authThrottles_.find(key);
    if (found == authThrottles_.end()) {
        return true;
    }
    found->second.lastSeenAt = now;
    return now >= found->second.nextAttemptAt;
}

void HttpWsServer::RecordAuthFailure(const std::string& remoteAddr) {
    const auto now = std::chrono::steady_clock::now();
    const std::string key = remoteAddr.empty() ? "<unknown>" : remoteAddr;
    std::lock_guard<std::mutex> lk(authMu_);
    if (authThrottles_.size() >= kMaxAuthThrottleEntries &&
        authThrottles_.find(key) == authThrottles_.end()) {
        // 来源地址极多时丢弃最旧项而不是无限增长；此次错误仍会被拒绝，
        // 只是不会为它保留下一次退避状态。
        auto oldest = std::min_element(authThrottles_.begin(), authThrottles_.end(),
            [](const auto& left, const auto& right) {
                return left.second.lastSeenAt < right.second.lastSeenAt;
            });
        if (oldest != authThrottles_.end()) {
            authThrottles_.erase(oldest);
        }
    }
    AuthThrottle& throttle = authThrottles_[key];
    throttle.failures = std::min(throttle.failures + 1, 6);
    const int backoffSeconds = 1 << (throttle.failures - 1);
    throttle.nextAttemptAt = now +
        std::chrono::seconds(backoffSeconds);
    throttle.lastSeenAt = now;
}

void HttpWsServer::ResetAuthFailures(const std::string& remoteAddr) {
    std::lock_guard<std::mutex> lk(authMu_);
    authThrottles_.erase(remoteAddr.empty() ? "<unknown>" : remoteAddr);
}

bool HttpWsServer::Start(const std::string& host, int port) {
    // 远控报文很小且延迟敏感，禁用 Nagle；同时限制 HTTP 请求体，避免未使用的
    // 上传路径被大请求占用内存。WebSocket 报文上限由 CMake 宏单独收紧。
    stopping_.store(false, std::memory_order_release);
    svr_.set_tcp_nodelay(true);
    svr_.set_payload_max_length(64 * 1024);
    // 发送超时保护发送线程，避免失联浏览器长期占用 socket 写操作。
    svr_.set_write_timeout(2, 0);
    // httplib 在真正进入 listen_internal 后才调用 start handler；用它而不是创建
    // std::thread 后立即置位，确保 running_ 与实际 accept 循环一致。
    svr_.set_start_handler([this] { running_ = true; });
    svr_.WebSocket("/ws", [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        const std::string remoteAddr = req.remote_addr;
        if (broadcaster_.Count() != 0) {
            ws.send("{\"t\":\"error\",\"msg\":\"controller already connected\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "controller already connected");
            return;
        }
        if (!TryBeginAuthAttempt()) {
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"server busy\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "server busy");
            return;
        }
        AuthAttemptGuard authAttempt(*this);
        ActiveWebSocketGuard socketGuard(*this, ws);
        if (!socketGuard.Registered()) {
            ws.close(httplib::ws::CloseStatus::GoingAway, "server stopping");
            return;
        }
        // 鉴权:第一帧必须是文本 JSON {"t":"auth","token":"..."}
        std::string msg;
        const auto r = ws.read(msg);
        if (r != httplib::ws::Text) {
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"need auth first\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "no auth");
            return;
        }
        if (!CanAttemptAuth(remoteAddr)) {
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"retry later\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "retry later");
            return;
        }
        bool ok = false;
        try {
            auto j = nlohmann::json::parse(msg, nullptr, false);
            if (j.is_object() && j.value("t", std::string()) == "auth") {
                const std::string token = j.value("token", std::string());
                if (authVerifier_ && authVerifier_(token)) {
                    ok = true;
                }
            }
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            RecordAuthFailure(remoteAddr);
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"bad token\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "bad token");
            return;
        }
        ResetAuthFailures(remoteAddr);
        if (stopping_.load(std::memory_order_acquire)) {
            ws.close(httplib::ws::CloseStatus::GoingAway, "server stopping");
            return;
        }
        ws.send("{\"t\":\"auth\",\"ok\":true}");
        if (cfgProvider_) {
            ws.send(cfgProvider_());
        }
        if (!broadcaster_.Add(&ws)) {
            ws.send("{\"t\":\"error\",\"msg\":\"controller already connected\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "controller already connected");
            return;
        }
        // 在 controller 真正注册前保持认证槽位，避免两个并发握手都收到
        // auth ok 后才发现控制端已满。
        authAttempt.Complete();
        ControllerSessionGuard controllerGuard(broadcaster_, ws, onControllerDisconnected_);
        controllerGuard.Arm();
        log::Info("ws client connected, total=" + std::to_string(broadcaster_.Count()));

        // 输入消息循环:浏览器回传的键鼠事件 JSON 在此转交 agent 处理。
        for (;;) {
            std::string m;
            const auto rr = ws.read(m);
            if (rr == httplib::ws::Fail || !ws.is_open()) {
                break;
            }
            if (rr == httplib::ws::Text) {
                const auto message = nlohmann::json::parse(m, nullptr, false);
                if (message.is_object()) {
                    const auto type = message.find("t");
                    const auto id = message.find("id");
                    if (type != message.end() && type->is_string() &&
                        type->get_ref<const std::string&>() == "ack" &&
                        id != message.end() && id->is_number_unsigned()) {
                        broadcaster_.AcknowledgeFrame(id->get<uint64_t>());
                        continue;
                    }
                }
            }
            if (onMessage_) {
                try {
                    onMessage_(m);
                } catch (const std::exception& e) {
                    // 单个损坏输入报文不能让 handler 跳过 broadcaster_.Remove。
                    log::Warn(std::string("ws input callback rejected message: ") + e.what());
                } catch (...) {
                    log::Warn("ws input callback rejected message with unknown exception");
                }
            }
        }
    });

    if (!svr_.bind_to_port(host, port)) {
        log::Error("bind failed on " + host + ":" + std::to_string(port));
        return false;
    }

    running_ = false;
    thread_ = std::thread([this, host, port]() {
        if (!svr_.listen_after_bind()) {
            log::Error("listen failed on " + host + ":" + std::to_string(port));
        }
        running_ = false;
    });

    // bind 成功只说明端口已占用到本进程；listen_after_bind 的 accept 循环尚未一定
    // 开始。等待 httplib 报告运行状态，避免 Agent 过早置 readyEvent 而控制端连接失败。
    svr_.wait_until_ready();
    while (!running_.load() && svr_.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!running_.load()) {
        log::Error("http/ws server failed before entering accept loop on " + host + ":" +
                   std::to_string(port));
        if (thread_.joinable()) {
            thread_.join();
        }
        return false;
    }
    log::Info("http/ws server listening on " + host + ":" + std::to_string(port));
    return true;
}

void HttpWsServer::Stop() {
    stopping_.store(true, std::memory_order_release);
    // 必须在 svr_.stop/join 之前关闭长连接。httplib 只停止 accept 循环，若 handler
    // 仍在 ws.read 中等待浏览器消息，listen 线程销毁线程池时会一直等待该任务。
    CloseActiveSocketForStop();
    svr_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    broadcaster_.Stop();
    running_ = false;
}

}  // namespace remote_assist
