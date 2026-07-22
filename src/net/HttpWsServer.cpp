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
        broadcaster_.Remove(&ws_);
        try {
            if (onDisconnected_) {
                onDisconnected_();
            }
        } catch (...) {
            // 析构函数绝不能因业务回调异常终止 WebSocket 工作线程。
            log::Error("controller disconnect callback threw an exception");
        }
        log::Info("ws client disconnected");
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

WsBroadcaster::WsBroadcaster()
    : senderThread_(&WsBroadcaster::SendLoop, this) {}

WsBroadcaster::~WsBroadcaster() {
    Stop();
}

bool WsBroadcaster::Add(httplib::ws::WebSocket* ws) {
    std::lock_guard<std::mutex> lk(clientMu_);
    if (client_ && client_->is_open()) {
        return false;
    }
    client_ = ws;
    return true;
}

void WsBroadcaster::Remove(httplib::ws::WebSocket* ws) {
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
        inFlightFrames_.clear();
        frameCv_.notify_all();
    }
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

void WsBroadcaster::BroadcastText(const std::string& msg) {
    std::lock_guard<std::mutex> lk(clientMu_);
    if (client_ && client_->is_open()) {
        client_->send(msg);
    }
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
        frameCv_.notify_one();
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
        uint64_t streamId = 0;
        uint64_t frameId = 0;
        uint64_t timestampUs = 0;
        bool isKeyFrame = true;
        {
            std::unique_lock<std::mutex> lk(frameMu_);
            for (;;) {
                if (stopping_) {
                    return;
                }
                if (inFlightFrames_.size() < kMaxFramesInFlight && !pendingFrame_.empty()) {
                    frame.swap(pendingFrame_);
                    streamId = pendingStreamId_;
                    pendingStreamId_ = 0;
                    isKeyFrame = pendingKeyFrame_;
                    pendingKeyFrame_ = true;
                    timestampUs = pendingTimestampUs_;
                    pendingTimestampUs_ = 0;
                    frameId = nextFrameId_++;
                    inFlightFrames_.push_back({frameId, std::chrono::steady_clock::now()});
                    break;
                }
                if (!inFlightFrames_.empty()) {
                    const auto deadline = inFlightFrames_.front().sentAt + kFrameAckTimeout;
                    if (frameCv_.wait_until(lk, deadline, [this] {
                            return stopping_ ||
                                (inFlightFrames_.size() < kMaxFramesInFlight &&
                                 !pendingFrame_.empty());
                        })) {
                        continue;
                    }
                    // 连接可能已半开，或者控制端页面在图片解码中崩溃。释放最早
                    // 超时帧的一个窗口配额；迟到确认按 id 匹配，不会影响新帧。
                    if (!inFlightFrames_.empty() &&
                        std::chrono::steady_clock::now() >=
                            inFlightFrames_.front().sentAt + kFrameAckTimeout) {
                        log::Warn("frame ack timed out, releasing one in-flight slot");
                        ackTimeouts_.fetch_add(1, std::memory_order_relaxed);
                        inFlightFrames_.pop_front();
                    }
                    continue;
                }
                frameCv_.wait(lk, [this] {
                    return stopping_ ||
                        (inFlightFrames_.size() < kMaxFramesInFlight && !pendingFrame_.empty());
                });
            }
        }

        bool sent = false;
        {
            std::lock_guard<std::mutex> lk(clientMu_);
            if (client_ && client_->is_open()) {
                const std::string header = "{\"t\":\"frame\",\"id\":" +
                    std::to_string(frameId) + ",\"stream_id\":" + std::to_string(streamId) +
                    ",\"key\":" + (isKeyFrame ? "true" : "false") +
                    ",\"ts\":" + std::to_string(timestampUs) + "}";
                sent = client_->send(header) &&
                    client_->send(reinterpret_cast<const char*>(frame.data()), frame.size());
            }
        }
        if (sent) {
            sentFrames_.fetch_add(1, std::memory_order_relaxed);
            sentBytes_.fetch_add(static_cast<uint64_t>(frame.size()), std::memory_order_relaxed);
        }
        if (!sent) {
            sendFailures_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> frameLock(frameMu_);
            const auto it = std::find_if(inFlightFrames_.begin(), inFlightFrames_.end(),
                [frameId](const InFlightFrame& inFlight) { return inFlight.id == frameId; });
            if (it != inFlightFrames_.end()) {
                inFlightFrames_.erase(it);
                frameCv_.notify_one();
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
    if (authAttemptInProgress_) {
        return false;
    }
    authAttemptInProgress_ = true;
    return true;
}

void HttpWsServer::EndAuthAttempt() {
    std::lock_guard<std::mutex> lk(authMu_);
    authAttemptInProgress_ = false;
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
    svr_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    broadcaster_.Stop();
    running_ = false;
}

}  // namespace remote_assist
