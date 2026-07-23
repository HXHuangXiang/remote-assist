#include "net/HttpWsServer.h"

#include "common/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

namespace remote_assist {

namespace {

// 浏览器收到 WebSocket 二进制帧后还要经历图片解码和 canvas 绘制。H.264 若把
// 偶发 rAF/解码抖动直接视作链路失效，会不断重建 GOP，体感反而更卡；因此采用
// 350~1000ms 的自适应确认窗口，首帧使用偏保守的 600ms，随后按真实 ACK 收敛。
constexpr auto kJpegFrameAckTimeout = std::chrono::milliseconds(300);
constexpr size_t kMaxPendingTextMessages = 8;
constexpr auto kAuthThrottleRetention = std::chrono::minutes(10);
constexpr size_t kMaxAuthThrottleEntries = 64;
constexpr size_t kMaxConcurrentAuthAttempts = 3;
// 浏览器在 WebSocket 打开后会立即发送认证 JSON。首帧使用绝对截止时间，避免
// 空连接或只发 Ping/Pong 的对端长期占住认证工作槽位；认证成功后恢复既有的
// 5 秒底层读取时限，库级 2 秒 heartbeat 会通过 Pong 保持正常空闲会话。
constexpr auto kAuthFirstFrameTimeout = std::chrono::seconds(2);
constexpr time_t kControllerReadTimeoutSeconds = 5;

std::string AuthAddressKey(const std::string& remoteAddr) {
    return remoteAddr.empty() ? "<unknown>" : remoteAddr;
}

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
// 都会释放来源对应的认证槽位，避免空连接耗尽 httplib 工作线程。
class HttpWsServer::AuthAttemptGuard {
public:
    AuthAttemptGuard(HttpWsServer& server, std::string remoteAddr)
        : server_(server), remoteAddr_(std::move(remoteAddr)) {}
    ~AuthAttemptGuard() {
        if (active_) {
            server_.EndAuthAttempt(remoteAddr_);
        }
    }

    void Complete() {
        if (active_) {
            server_.EndAuthAttempt(remoteAddr_);
            active_ = false;
        }
    }

private:
    HttpWsServer& server_;
    std::string remoteAddr_;
    bool active_ = true;
};

// WebSocket 对象由 httplib handler 在栈上持有，不能把裸指针复制到 Stop 之后再
// 使用。guard 的析构与 AbortActiveSocketsForStop 使用同一把锁，因此 Stop 持锁
// 关闭底层 socket 时，handler 不会抢先返回并销毁对象。
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
    if (client_ || !controllerHandoff_.TryAttach()) {
        return false;
    }
    client_ = ws;
    pendingFrame_.clear();
    pendingTiles_.clear();
    pendingStreamId_ = 0;
    pendingTimestampUs_ = 0;
    pendingKeyFrame_ = true;
    pendingH264_ = false;
    pendingPatch_ = false;
    pendingPatchBaseline_ = false;
    pendingText_.clear();
    pendingReplaceableText_.clear();
    preferReplaceableText_ = false;
    inFlightFrames_.clear();
    h264AckTimeoutWindowMs_ = kH264AckTimeoutInitialMs;
    h264AckTimeoutInitialized_ = false;
    h264AckTimeoutMs_.store(h264AckTimeoutWindowMs_, std::memory_order_relaxed);
    h264ResyncRequested_.store(false, std::memory_order_release);
    patchResyncRequested_.store(false, std::memory_order_release);
    frameCv_.notify_all();
    controllerCv_.notify_all();
    return true;
}

bool WsBroadcaster::Remove(httplib::ws::WebSocket* ws) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(clientMu_);
        if (client_ == ws) {
            client_ = nullptr;
            controllerHandoff_.Detach();
            removed = true;
        }
    }
    if (removed) {
        std::lock_guard<std::mutex> lk(frameMu_);
        // 新控制端连接后由采集线程生成首帧；不要把断连前的旧图像带过去。
        pendingFrame_.clear();
        pendingTiles_.clear();
        pendingStreamId_ = 0;
        pendingTimestampUs_ = 0;
        pendingKeyFrame_ = true;
        pendingH264_ = false;
        pendingPatch_ = false;
        pendingPatchBaseline_ = false;
        pendingText_.clear();
        pendingReplaceableText_.clear();
        preferReplaceableText_ = false;
        inFlightFrames_.clear();
        h264ResyncRequested_.store(false, std::memory_order_release);
        patchResyncRequested_.store(false, std::memory_order_release);
        frameCv_.notify_all();
    }
    controllerCv_.notify_all();
    return removed;
}

void WsBroadcaster::BeginControllerInputCleanup() {
    std::lock_guard<std::mutex> lk(clientMu_);
    controllerHandoff_.BeginInputCleanup();
}

void WsBroadcaster::CompleteControllerInputCleanup() {
    std::lock_guard<std::mutex> lk(clientMu_);
    controllerHandoff_.CompleteInputCleanup();
}

bool WsBroadcaster::CanAcceptController() {
    std::lock_guard<std::mutex> lk(clientMu_);
    return !client_ && controllerHandoff_.CanAccept();
}

bool WsBroadcaster::IsControllerInputCleanupPending() {
    std::lock_guard<std::mutex> lk(clientMu_);
    return controllerHandoff_.IsInputCleanupPending();
}

bool WsBroadcaster::WaitForActiveController(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(clientMu_);
    const auto isActive = [this] {
        return client_ && client_->is_open() && controllerHandoff_.HasActiveController();
    };
    return controllerCv_.wait_for(lk, timeout, isActive) && isActive();
}

FrameQueueResult WsBroadcaster::BroadcastBinary(std::vector<uint8_t> frame,
                                                uint64_t streamId, bool isKeyFrame,
                                                uint64_t timestampUs, bool h264,
                                                bool patchBaseline) {
    bool replaced = false;
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        if (stopping_) {
            return FrameQueueResult::kStopped;
        }
        // 图块超时后只有采集线程完成流版本切换、请求新的完整关键帧后才能恢复。
        // 否则一张恰好并发入队的普通 JPEG/H.264 帧会越过重同步边界，重新成为
        // 不可信画布上的“基线”。
        if (patchResyncRequested_.load(std::memory_order_acquire)) {
            return FrameQueueResult::kPatchResyncRequired;
        }
        // ACK 超时已经宣布旧 GOP 失效时，采集线程可能刚好在一次信用等待中醒来。
        // 在这里再次拒绝 delta，确保超时后的下一条视频负载只能是新的 IDR。
        if (H264DeltaRequiresResync(h264, isKeyFrame, false,
                                    h264ResyncRequested_.load(std::memory_order_acquire))) {
            return FrameQueueResult::kH264ResyncRequired;
        }
        if (pendingPatch_) {
            // 图块批次不能被整帧静默覆盖。浏览器可能已经绘制了其中一部分，只有
            // 新的完整基线才能重新建立一致画面。
            pendingTiles_.clear();
            pendingPatch_ = false;
            patchResyncRequested_.store(true, std::memory_order_release);
            return FrameQueueResult::kPatchResyncRequired;
        }
        if (!pendingFrame_.empty()) {
            // JPEG 帧没有前后依赖，直接替换可把延迟保持在一帧以内。H.264
            // delta 帧则会引用被替换的上一帧，若仍把最新 delta 发给浏览器会
            // 触发解码失败/黑屏。此时清空待发送帧，并让 Agent 请求新的 IDR。
            if (H264DeltaRequiresResync(h264, isKeyFrame, true, false)) {
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
        pendingH264_ = h264;
        pendingPatch_ = false;
        pendingPatchBaseline_ = patchBaseline;
        pendingTimestampUs_ = timestampUs;
        queuedFrames_.fetch_add(1, std::memory_order_relaxed);
    }
    frameCv_.notify_one();
    return replaced ? FrameQueueResult::kReplaced : FrameQueueResult::kQueued;
}

FrameQueueResult WsBroadcaster::BroadcastPatch(std::vector<JpegTile> tiles,
                                               uint64_t streamId, uint64_t timestampUs) {
    if (tiles.empty()) {
        return FrameQueueResult::kStopped;
    }
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        if (stopping_) {
            return FrameQueueResult::kStopped;
        }
        if (patchResyncRequested_.load(std::memory_order_acquire)) {
            return FrameQueueResult::kPatchResyncRequired;
        }
        // 局部更新依赖前一张已绘制画面。只要队列或窗口中还有视频更新，当前
        // 批次就不能覆盖它，否则页面会缺少中间像素而永久不同步。
        if (!pendingFrame_.empty() || pendingPatch_ || !inFlightFrames_.empty()) {
            patchResyncRequested_.store(true, std::memory_order_release);
            return FrameQueueResult::kPatchResyncRequired;
        }
        pendingTiles_ = std::move(tiles);
        pendingStreamId_ = streamId;
        pendingTimestampUs_ = timestampUs;
        pendingKeyFrame_ = false;
        pendingH264_ = false;
        pendingPatch_ = true;
        pendingPatchBaseline_ = false;
        queuedFrames_.fetch_add(1, std::memory_order_relaxed);
    }
    frameCv_.notify_one();
    return FrameQueueResult::kQueued;
}

bool WsBroadcaster::ConsumeH264ResyncRequest() {
    return h264ResyncRequested_.exchange(false, std::memory_order_acq_rel);
}

bool WsBroadcaster::ConsumePatchResyncRequest() {
    return patchResyncRequested_.exchange(false, std::memory_order_acq_rel);
}

bool WsBroadcaster::WaitForH264FrameCredit(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(frameMu_);
    const bool ready = frameCv_.wait_for(lk, timeout, [this] {
        // pendingFrame_ 也属于端到端延迟窗口。旧逻辑只限制 in-flight，因而会
        // 额外保留一张“尚未写入 socket”的 delta，在慢一拍的浏览器上等同于多
        // 一帧可见延迟。两个槽位已足以让网络写、硬件解码和下一帧编码并行。
        // ACK 超时要求重建 GOP 时也要立刻退出，交由采集线程先消费重同步请求。
        const bool resyncRequested = h264ResyncRequested_.load(std::memory_order_acquire);
        const bool patchInFlight = std::any_of(inFlightFrames_.begin(), inFlightFrames_.end(),
            [](const InFlightFrame& frame) { return frame.patch; });
        return stopping_ || resyncRequested ||
            (!patchInFlight && HasH264FrameCredit(
                !pendingFrame_.empty() || pendingPatch_, inFlightFrames_.size(), false, false));
    });
    const bool patchInFlight = std::any_of(inFlightFrames_.begin(), inFlightFrames_.end(),
        [](const InFlightFrame& frame) { return frame.patch; });
    return ready && !patchInFlight && HasH264FrameCredit(
        !pendingFrame_.empty() || pendingPatch_, inFlightFrames_.size(),
        h264ResyncRequested_.load(std::memory_order_acquire), stopping_);
}

bool WsBroadcaster::WaitForPatchFrameCredit(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(frameMu_);
    const bool ready = frameCv_.wait_for(lk, timeout, [this] {
        return stopping_ || patchResyncRequested_.load(std::memory_order_acquire) ||
            (pendingFrame_.empty() && !pendingPatch_ && inFlightFrames_.empty());
    });
    return ready && !stopping_ && !patchResyncRequested_.load(std::memory_order_acquire) &&
        pendingFrame_.empty() && !pendingPatch_ && inFlightFrames_.empty();
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
        // 二进制负载尚在 send 调用中时，先保留 ACK。发送线程会在真正完成写入
        // 后处理它，避免把写入耗时算进浏览器端 ACK 延迟，也避免 ACK 丢失导致
        // 后续白白等一个完整超时窗口。
        if (!it->writeCompleted) {
            it->ackedDuringWrite = true;
            frameCv_.notify_all();
            return;
        }
        const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - it->sentAt).count();
        const bool h264 = it->h264;
        inFlightFrames_.erase(it);
        acknowledgedFrames_.fetch_add(1, std::memory_order_relaxed);
        ackLatencyUs_.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, latency)),
                                std::memory_order_relaxed);
        ackLatencySamples_.fetch_add(1, std::memory_order_relaxed);
        if (h264) {
            const uint64_t observedMs = static_cast<uint64_t>(std::max<int64_t>(0, latency)) /
                1000;
            // 保留网络与下一次 rAF 的余量。首个真实样本直接收敛，后续样本采用
            // 1/4 步长平滑，避免一次 GC/关键帧将窗口长期推到上限。
            h264AckTimeoutWindowMs_ = NextH264AckTimeoutAfterAck(
                h264AckTimeoutWindowMs_, h264AckTimeoutInitialized_, observedMs);
            h264AckTimeoutInitialized_ = true;
            h264AckTimeoutMs_.store(h264AckTimeoutWindowMs_, std::memory_order_relaxed);
        }
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
    stats.h264AckTimeoutMs = h264AckTimeoutMs_.load(std::memory_order_relaxed);
    stats.sendFailures = sendFailures_.load(std::memory_order_relaxed);
    stats.h264Resyncs = h264Resyncs_.load(std::memory_order_relaxed);
    return stats;
}

int WsBroadcaster::Count() {
    std::lock_guard<std::mutex> lk(clientMu_);
    return (client_ && client_->is_open() && controllerHandoff_.HasActiveController()) ? 1 : 0;
}

void WsBroadcaster::Stop() {
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        pendingFrame_.clear();
        pendingTiles_.clear();
        pendingStreamId_ = 0;
        pendingTimestampUs_ = 0;
        pendingKeyFrame_ = true;
        pendingH264_ = false;
        pendingPatch_ = false;
        pendingPatchBaseline_ = false;
        pendingText_.clear();
        pendingReplaceableText_.clear();
        preferReplaceableText_ = false;
        inFlightFrames_.clear();
        h264ResyncRequested_.store(false, std::memory_order_release);
        patchResyncRequested_.store(false, std::memory_order_release);
    }
    frameCv_.notify_all();
    if (senderThread_.joinable()) {
        senderThread_.join();
    }
}

void WsBroadcaster::SendLoop() {
    for (;;) {
        std::vector<uint8_t> frame;
        std::vector<JpegTile> tiles;
        std::string text;
        uint64_t streamId = 0;
        uint64_t frameId = 0;
        uint64_t timestampUs = 0;
        bool isKeyFrame = true;
        bool h264 = false;
        bool patch = false;
        bool sendingFrame = false;
        {
            std::unique_lock<std::mutex> lk(frameMu_);
            for (;;) {
                if (stopping_) {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                const auto expired = std::find_if(inFlightFrames_.begin(), inFlightFrames_.end(),
                    [now](const InFlightFrame& frame) {
                        return frame.writeCompleted && now >= frame.ackDeadline;
                    });
                if (expired != inFlightFrames_.end()) {
                    // 即使持续有 cursor 消息，也必须按时释放超时帧窗口；否则高频
                    // 指针移动会让 H.264 信用窗口永久卡在满载状态。
                    const bool h264Timeout = expired->h264;
                    const bool patchTimeout = expired->patch || expired->patchBaseline;
                    ackTimeouts_.fetch_add(1, std::memory_order_relaxed);
                    inFlightFrames_.erase(expired);
                    if (patchTimeout) {
                        // 图块批次或其完整基线未在时限内完成合成时，继续发送下一批
                        // 会让浏览器失去中间像素。清空队列并请求新的完整关键帧。
                        pendingFrame_.clear();
                        pendingTiles_.clear();
                        pendingStreamId_ = 0;
                        pendingTimestampUs_ = 0;
                        pendingKeyFrame_ = true;
                        pendingH264_ = false;
                        pendingPatch_ = false;
                        pendingPatchBaseline_ = false;
                        inFlightFrames_.clear();
                        patchResyncRequested_.store(true, std::memory_order_release);
                        log::Warn("patch baseline or batch ack timed out, requesting full-frame resync");
                        frameCv_.notify_all();
                    } else if (h264Timeout) {
                        // TCP 仍会按顺序交付已经写出的预测帧，但浏览器 300ms 未能
                        // 绘制时继续发送 P 帧只会放大陈旧画面。清空尚未发出的帧，
                        // 由采集线程请求新 IDR；已在 socket 缓冲中的旧帧无法撤回，
                        // 但随后的独立 GOP 可以让浏览器快速丢弃并重新建立参考链。
                        pendingFrame_.clear();
                        pendingTiles_.clear();
                        pendingStreamId_ = 0;
                        pendingTimestampUs_ = 0;
                        pendingKeyFrame_ = true;
                        pendingH264_ = false;
                        pendingPatch_ = false;
                        pendingPatchBaseline_ = false;
                        inFlightFrames_.clear();
                        h264Resyncs_.fetch_add(1, std::memory_order_relaxed);
                        h264ResyncRequested_.store(true, std::memory_order_release);
                        // 若首个慢帧尚未来得及返回 ACK，不能一直使用同一个阈值
                        // 重复打断 GOP。逐次放宽到上限，真实样本回来后再平滑收敛。
                        const uint32_t previousWindow = h264AckTimeoutWindowMs_;
                        h264AckTimeoutWindowMs_ =
                            NextH264AckTimeoutAfterTimeout(previousWindow);
                        h264AckTimeoutInitialized_ = true;
                        h264AckTimeoutMs_.store(h264AckTimeoutWindowMs_,
                                                std::memory_order_relaxed);
                        log::Warn("H.264 frame ack timed out, requesting IDR resync; ack_window_ms=" +
                                  std::to_string(h264AckTimeoutWindowMs_));
                        frameCv_.notify_all();
                    } else {
                        log::Warn("frame ack timed out, releasing one in-flight slot");
                    }
                    continue;
                }
                if (!pendingText_.empty()) {
                    text = std::move(pendingText_.front());
                    pendingText_.pop_front();
                    break;
                }
                const bool hasPendingFrame = pendingPatch_ ? !pendingTiles_.empty() :
                    !pendingFrame_.empty();
                const bool canSendFrame = hasPendingFrame &&
                    ((pendingPatch_ || pendingPatchBaseline_) ? inFlightFrames_.empty() :
                     inFlightFrames_.size() < kFrameWindowCapacity);
                if (!pendingReplaceableText_.empty() &&
                    (!canSendFrame || preferReplaceableText_)) {
                    text = std::move(pendingReplaceableText_);
                    pendingReplaceableText_.clear();
                    preferReplaceableText_ = false;
                    break;
                }
                if (canSendFrame) {
                    patch = pendingPatch_;
                    if (patch) {
                        tiles.swap(pendingTiles_);
                    } else {
                        frame.swap(pendingFrame_);
                    }
                    streamId = pendingStreamId_;
                    pendingStreamId_ = 0;
                    isKeyFrame = pendingKeyFrame_;
                    pendingKeyFrame_ = true;
                    h264 = pendingH264_;
                    pendingH264_ = false;
                    const bool patchBaseline = pendingPatchBaseline_;
                    pendingPatchBaseline_ = false;
                    pendingPatch_ = false;
                    timestampUs = pendingTimestampUs_;
                    pendingTimestampUs_ = 0;
                    frameId = nextFrameId_++;
                    // 先预留窗口，二进制负载成功写入后才将 writeCompleted 置为
                    // true 并记录 sentAt。这样网络 send 阻塞不会侵蚀浏览器的
                    // 解码/绘制 ACK 时间预算。
                    InFlightFrame inFlight;
                    inFlight.id = frameId;
                    inFlight.h264 = h264;
                    inFlight.patch = patch;
                    inFlight.patchBaseline = patchBaseline;
                    inFlightFrames_.push_back(std::move(inFlight));
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
                    const auto sent = std::find_if(inFlightFrames_.begin(), inFlightFrames_.end(),
                        [](const InFlightFrame& frame) { return frame.writeCompleted; });
                    if (sent == inFlightFrames_.end()) {
                        // 正常情况下发送线程不会在还有写入中帧时重新进入调度循环；
                        // 保留此分支用于关闭/未来改造场景，避免用默认时间点立即超时。
                        frameCv_.wait(lk, [this] {
                            return stopping_ || std::any_of(inFlightFrames_.begin(),
                                inFlightFrames_.end(), [](const InFlightFrame& frame) {
                                    return frame.writeCompleted;
                                });
                        });
                        continue;
                    }
                    const auto deadline = sent->ackDeadline;
                    if (frameCv_.wait_until(lk, deadline, [this] {
                        return stopping_ ||
                            !pendingText_.empty() || !pendingReplaceableText_.empty() ||
                            (((pendingPatch_ || pendingPatchBaseline_) && inFlightFrames_.empty() &&
                              (pendingPatch_ ? !pendingTiles_.empty() : !pendingFrame_.empty())) ||
                             (!pendingPatch_ && !pendingPatchBaseline_ &&
                              inFlightFrames_.size() < kFrameWindowCapacity &&
                              !pendingFrame_.empty()));
                        })) {
                        continue;
                    }
                    continue;
                }
                frameCv_.wait(lk, [this] {
                    return stopping_ ||
                        !pendingText_.empty() || !pendingReplaceableText_.empty() ||
                        (((pendingPatch_ || pendingPatchBaseline_) && inFlightFrames_.empty() &&
                          (pendingPatch_ ? !pendingTiles_.empty() : !pendingFrame_.empty())) ||
                         (!pendingPatch_ && !pendingPatchBaseline_ &&
                          inFlightFrames_.size() < kFrameWindowCapacity &&
                          !pendingFrame_.empty()));
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
                } else if (!patch) {
                    const std::string header = "{\"t\":\"frame\",\"id\":" +
                        std::to_string(frameId) + ",\"stream_id\":" + std::to_string(streamId) +
                        ",\"key\":" + (isKeyFrame ? "true" : "false") +
                        ",\"ts\":" + std::to_string(timestampUs) + "}";
                    sent = client_->send(header) &&
                        client_->send(reinterpret_cast<const char*>(frame.data()), frame.size());
                } else {
                    const std::string header = "{\"t\":\"patch\",\"id\":" +
                        std::to_string(frameId) + ",\"stream_id\":" + std::to_string(streamId) +
                        ",\"count\":" + std::to_string(tiles.size()) + ",\"ts\":" +
                        std::to_string(timestampUs) + "}";
                    sent = client_->send(header);
                    for (size_t index = 0; sent && index < tiles.size(); ++index) {
                        const JpegTile& tile = tiles[index];
                        const std::string tileHeader = "{\"t\":\"tile\",\"id\":" +
                            std::to_string(frameId) + ",\"i\":" + std::to_string(index) +
                            ",\"x\":" + std::to_string(tile.x) + ",\"y\":" +
                            std::to_string(tile.y) + ",\"w\":" + std::to_string(tile.width) +
                            ",\"h\":" + std::to_string(tile.height) + "}";
                        sent = client_->send(tileHeader) &&
                            client_->send(reinterpret_cast<const char*>(tile.data.data()),
                                          tile.data.size());
                    }
                }
                if (!sent && client_->is_open()) {
                    // 视频帧由文本元数据和紧随其后的二进制负载组成。若前者成功、
                    // 后者失败仍继续复用同一 WebSocket，浏览器可能把下一段负载
                    // 绑定到旧 frame id，随后 ACK/解码状态都会错位。发送失败时
                    // 立即中断会话，由现有断连逻辑清空队列并在重连后从 IDR 开始。
                    // 发送线程与 handler 的 ws.read 并行，不能调用会同步读取 Close
                    // 响应的 close()，否则会再次竞争同一 socket 的接收方向。
                    client_->abort();
                }
            }
        }
        if (sent && sendingFrame) {
            const auto writeCompletedAt = std::chrono::steady_clock::now();
            bool acknowledgedDuringWrite = false;
            {
                std::lock_guard<std::mutex> frameLock(frameMu_);
                const auto it = std::find_if(inFlightFrames_.begin(), inFlightFrames_.end(),
                    [frameId](const InFlightFrame& inFlight) {
                        return inFlight.id == frameId;
                    });
                if (it != inFlightFrames_.end()) {
                    if (it->ackedDuringWrite) {
                        // 浏览器已经确认该帧；它的真实延迟不应被发送线程的写入
                        // 时长污染，因此只累计确认次数，不纳入 ACK 时延均值。
                        inFlightFrames_.erase(it);
                        acknowledgedDuringWrite = true;
                    } else {
                        it->writeCompleted = true;
                        it->sentAt = writeCompletedAt;
                        it->ackDeadline = writeCompletedAt +
                            ((it->patch || it->patchBaseline) ? std::chrono::milliseconds(1000)
                             : (it->h264
                                ? std::chrono::milliseconds(h264AckTimeoutWindowMs_)
                                : kJpegFrameAckTimeout));
                    }
                    frameCv_.notify_all();
                }
            }
            sentFrames_.fetch_add(1, std::memory_order_relaxed);
            uint64_t sentBytes = static_cast<uint64_t>(frame.size());
            for (const auto& tile : tiles) {
                sentBytes += static_cast<uint64_t>(tile.data.size());
            }
            sentBytes_.fetch_add(sentBytes, std::memory_order_relaxed);
            if (acknowledgedDuringWrite) {
                acknowledgedFrames_.fetch_add(1, std::memory_order_relaxed);
            }
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

bool HttpWsServer::TryBeginAuthAttempt(const std::string& remoteAddr) {
    std::lock_guard<std::mutex> lk(authMu_);
    if (stopping_.load(std::memory_order_acquire) ||
        authAttemptsByAddress_.size() >= kMaxConcurrentAuthAttempts) {
        return false;
    }
    const std::string key = AuthAddressKey(remoteAddr);
    if (authAttemptsByAddress_.find(key) != authAttemptsByAddress_.end()) {
        return false;
    }
    authAttemptsByAddress_.emplace(key, 1);
    return true;
}

void HttpWsServer::EndAuthAttempt(const std::string& remoteAddr) {
    std::lock_guard<std::mutex> lk(authMu_);
    authAttemptsByAddress_.erase(AuthAddressKey(remoteAddr));
}

bool HttpWsServer::RegisterActiveSocket(httplib::ws::WebSocket* ws) {
    if (!ws) {
        return false;
    }
    std::lock_guard<std::mutex> lk(connectionMu_);
    if (stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    return activeSockets_.insert(ws).second;
}

void HttpWsServer::UnregisterActiveSocket(httplib::ws::WebSocket* ws) {
    std::lock_guard<std::mutex> lk(connectionMu_);
    activeSockets_.erase(ws);
}

void HttpWsServer::AbortActiveSocketsForStop() {
    std::lock_guard<std::mutex> lk(connectionMu_);
    if (activeSockets_.empty()) {
        return;
    }
    // 不能在这里调用 WebSocket::close：handler 线程可能已经在 ws.read()，而
    // httplib 的 close() 会在发送 Close 帧后同步读取对端的 Close 响应，形成两个
    // 并发 reader。abort() 仅 shutdown 底层 socket，当前 read 立即失败、handler
    // 正常走自己的析构收尾；Server 随后会关闭实际 socket。
    log::Info("aborting " + std::to_string(activeSockets_.size()) +
              " active WebSocket connection(s) for server shutdown");
    for (auto* socket : activeSockets_) {
        if (socket) {
            socket->abort();
        }
    }
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
    const std::string key = AuthAddressKey(remoteAddr);
    const auto found = authThrottles_.find(key);
    if (found == authThrottles_.end()) {
        return true;
    }
    found->second.lastSeenAt = now;
    return now >= found->second.nextAttemptAt;
}

void HttpWsServer::RecordAuthFailure(const std::string& remoteAddr) {
    const auto now = std::chrono::steady_clock::now();
    const std::string key = AuthAddressKey(remoteAddr);
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
    authThrottles_.erase(AuthAddressKey(remoteAddr));
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
        if (!broadcaster_.CanAcceptController()) {
            const bool cleaningUp = broadcaster_.IsControllerInputCleanupPending();
            const char* const message = cleaningUp
                ? "controller input cleanup in progress" : "controller already connected";
            const std::string error = cleaningUp
                ? "{\"t\":\"error\",\"code\":\"controller_cleanup\","
                  "\"msg\":\"controller input cleanup in progress\"}"
                : "{\"t\":\"error\",\"msg\":\"controller already connected\"}";
            ws.send(error);
            ws.close(httplib::ws::CloseStatus::PolicyViolation, message);
            return;
        }
        if (!CanAttemptAuth(remoteAddr)) {
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"retry later\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "retry later");
            return;
        }
        if (!TryBeginAuthAttempt(remoteAddr)) {
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"server busy\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "server busy");
            return;
        }
        AuthAttemptGuard authAttempt(*this, remoteAddr);
        ActiveWebSocketGuard socketGuard(*this, ws);
        if (!socketGuard.Registered()) {
            ws.close(httplib::ws::CloseStatus::GoingAway, "server stopping");
            return;
        }
        // 鉴权:第一帧必须是文本 JSON {"t":"auth","token":"..."}
        std::string msg;
        const auto r = ws.read_with_timeout(msg, kAuthFirstFrameTimeout);
        if (r != httplib::ws::Text) {
            RecordAuthFailure(remoteAddr);
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"need auth first\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "no auth");
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
        // 认证阶段会把底层读取时限按剩余预算缩短；通过后恢复既有会话时限，由
        // WebSocket heartbeat 的 Pong 维持正常空闲页面。
        ws.set_read_timeout(kControllerReadTimeoutSeconds, 0);
        ws.send("{\"t\":\"auth\",\"ok\":true}");
        if (cfgProvider_) {
            ws.send(cfgProvider_());
        }
        if (!broadcaster_.Add(&ws)) {
            const bool cleaningUp = broadcaster_.IsControllerInputCleanupPending();
            const char* const message = cleaningUp
                ? "controller input cleanup in progress" : "controller already connected";
            const std::string error = cleaningUp
                ? "{\"t\":\"error\",\"code\":\"controller_cleanup\","
                  "\"msg\":\"controller input cleanup in progress\"}"
                : "{\"t\":\"error\",\"msg\":\"controller already connected\"}";
            ws.send(error);
            ws.close(httplib::ws::CloseStatus::PolicyViolation, message);
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
    // 必须在 svr_.stop/join 之前中断长连接。httplib 只停止 accept 循环，若 handler
    // 仍在 ws.read 中等待浏览器消息，listen 线程销毁线程池时会一直等待该任务。
    AbortActiveSocketsForStop();
    svr_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    broadcaster_.Stop();
    running_ = false;
}

}  // namespace remote_assist
