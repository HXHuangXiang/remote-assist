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

}  // namespace

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
        frameInFlight_ = false;
        inFlightFrameId_ = 0;
        inFlightSince_ = {};
        frameCv_.notify_all();
    }
}

void WsBroadcaster::BroadcastBinary(std::vector<uint8_t> frame, uint64_t streamId) {
    {
        std::lock_guard<std::mutex> lk(frameMu_);
        if (stopping_) {
            return;
        }
        pendingFrame_ = std::move(frame);
        pendingStreamId_ = streamId;
    }
    frameCv_.notify_one();
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
    if (frameInFlight_ && inFlightFrameId_ == frameId) {
        frameInFlight_ = false;
        inFlightFrameId_ = 0;
        inFlightSince_ = {};
        frameCv_.notify_one();
    }
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
        frameInFlight_ = false;
        inFlightFrameId_ = 0;
        inFlightSince_ = {};
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
        {
            std::unique_lock<std::mutex> lk(frameMu_);
            for (;;) {
                if (stopping_) {
                    return;
                }
                if (!frameInFlight_ && !pendingFrame_.empty()) {
                    frame.swap(pendingFrame_);
                    streamId = pendingStreamId_;
                    pendingStreamId_ = 0;
                    frameId = nextFrameId_++;
                    frameInFlight_ = true;
                    inFlightFrameId_ = frameId;
                    inFlightSince_ = std::chrono::steady_clock::now();
                    break;
                }
                if (frameInFlight_) {
                    const auto deadline = inFlightSince_ + kFrameAckTimeout;
                    if (frameCv_.wait_until(lk, deadline, [this] {
                            return stopping_ || !frameInFlight_;
                        })) {
                        continue;
                    }
                    // 连接可能已半开，或者控制端页面在图片解码中崩溃。允许使用
                    // 最新待发送帧继续尝试，且帧 id 单调递增避免迟到确认误释放新帧。
                    if (frameInFlight_) {
                        log::Warn("frame ack timed out, replacing in-flight frame");
                        frameInFlight_ = false;
                        inFlightFrameId_ = 0;
                        inFlightSince_ = {};
                    }
                    continue;
                }
                frameCv_.wait(lk, [this] {
                    return stopping_ || frameInFlight_ || !pendingFrame_.empty();
                });
            }
        }

        bool sent = false;
        {
            std::lock_guard<std::mutex> lk(clientMu_);
            if (client_ && client_->is_open()) {
                const std::string header = "{\"t\":\"frame\",\"id\":" +
                    std::to_string(frameId) + ",\"stream_id\":" + std::to_string(streamId) + "}";
                sent = client_->send(header) &&
                    client_->send(reinterpret_cast<const char*>(frame.data()), frame.size());
            }
        }
        if (!sent) {
            std::lock_guard<std::mutex> frameLock(frameMu_);
            if (frameInFlight_ && inFlightFrameId_ == frameId) {
                frameInFlight_ = false;
                inFlightFrameId_ = 0;
                inFlightSince_ = {};
                frameCv_.notify_one();
            }
        }
    }
}

HttpWsServer::HttpWsServer() = default;

HttpWsServer::~HttpWsServer() {
    Stop();
}

void HttpWsServer::SetWebDir(const std::string& dir) {
    svr_.set_mount_point("/", dir);
}

void HttpWsServer::SetAuthVerifier(AuthVerifier v) { authVerifier_ = std::move(v); }
void HttpWsServer::SetCfgProvider(CfgProvider p) { cfgProvider_ = std::move(p); }
void HttpWsServer::SetOnMessage(OnMessage cb) { onMessage_ = std::move(cb); }
void HttpWsServer::SetOnControllerDisconnected(OnControllerDisconnected cb) {
    onControllerDisconnected_ = std::move(cb);
}

bool HttpWsServer::CanAttemptAuth() {
    std::lock_guard<std::mutex> lk(authMu_);
    return std::chrono::steady_clock::now() >= nextAuthAt_;
}

void HttpWsServer::RecordAuthFailure() {
    std::lock_guard<std::mutex> lk(authMu_);
    authFailures_ = std::min(authFailures_ + 1, 6);
    const int backoffSeconds = 1 << (authFailures_ - 1);
    nextAuthAt_ = std::chrono::steady_clock::now() +
        std::chrono::seconds(backoffSeconds);
}

void HttpWsServer::ResetAuthFailures() {
    std::lock_guard<std::mutex> lk(authMu_);
    authFailures_ = 0;
    nextAuthAt_ = {};
}

bool HttpWsServer::Start(const std::string& host, int port) {
    // 远控报文很小且延迟敏感，禁用 Nagle；同时限制 HTTP 请求体，避免未使用的
    // 上传路径被大请求占用内存。WebSocket 报文上限由 CMake 宏单独收紧。
    svr_.set_tcp_nodelay(true);
    svr_.set_payload_max_length(64 * 1024);
    // 发送超时保护发送线程，避免失联浏览器长期占用 socket 写操作。
    svr_.set_write_timeout(2, 0);
    svr_.WebSocket("/ws", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
        // 鉴权:第一帧必须是文本 JSON {"t":"auth","token":"..."}
        std::string msg;
        const auto r = ws.read(msg);
        if (r != httplib::ws::Text) {
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"need auth first\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "no auth");
            return;
        }
        if (!CanAttemptAuth()) {
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
            RecordAuthFailure();
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"bad token\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "bad token");
            return;
        }
        ResetAuthFailures();
        ws.send("{\"t\":\"auth\",\"ok\":true}");
        if (cfgProvider_) {
            ws.send(cfgProvider_());
        }
        if (!broadcaster_.Add(&ws)) {
            ws.send("{\"t\":\"error\",\"msg\":\"controller already connected\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "controller already connected");
            return;
        }
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
                onMessage_(m);
            }
        }
        broadcaster_.Remove(&ws);
        if (onControllerDisconnected_) {
            onControllerDisconnected_();
        }
        log::Info("ws client disconnected");
    });

    if (!svr_.bind_to_port(host, port)) {
        log::Error("bind failed on " + host + ":" + std::to_string(port));
        return false;
    }

    running_ = true;
    thread_ = std::thread([this, host, port]() {
        if (!svr_.listen_after_bind()) {
            log::Error("listen failed on " + host + ":" + std::to_string(port));
            running_ = false;
            return;
        }
    });
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
