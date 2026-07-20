#include "net/HttpWsServer.h"

#include "common/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace remote_assist {

void WsBroadcaster::Add(httplib::ws::WebSocket* ws) {
    std::lock_guard<std::mutex> lk(mu_);
    clients_.push_back(ws);
}

void WsBroadcaster::Remove(httplib::ws::WebSocket* ws) {
    std::lock_guard<std::mutex> lk(mu_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), ws), clients_.end());
}

void WsBroadcaster::BroadcastBinary(const char* data, size_t len) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto* ws : clients_) {
        if (ws && ws->is_open()) {
            ws->send(data, len);
        }
    }
}

void WsBroadcaster::BroadcastText(const std::string& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto* ws : clients_) {
        if (ws && ws->is_open()) {
            ws->send(msg);
        }
    }
}

int WsBroadcaster::Count() {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(clients_.size());
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

bool HttpWsServer::Start(const std::string& host, int port) {
    svr_.WebSocket("/ws", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
        // 鉴权:第一帧必须是文本 JSON {"t":"auth","token":"..."}
        std::string msg;
        const auto r = ws.read(msg);
        if (r != httplib::ws::Text) {
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
            ws.send("{\"t\":\"auth\",\"ok\":false,\"reason\":\"bad token\"}");
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "bad token");
            return;
        }
        ws.send("{\"t\":\"auth\",\"ok\":true}");
        if (cfgProvider_) {
            ws.send(cfgProvider_());
        }
        broadcaster_.Add(&ws);
        log::Info("ws client connected, total=" + std::to_string(broadcaster_.Count()));

        // 输入消息循环:浏览器回传的键鼠事件 JSON 在此转交 agent 处理。
        for (;;) {
            std::string m;
            const auto rr = ws.read(m);
            if (rr == httplib::ws::Fail || !ws.is_open()) {
                break;
            }
            if (onMessage_) {
                onMessage_(m);
            }
        }
        broadcaster_.Remove(&ws);
        log::Info("ws client disconnected");
    });

    running_ = true;
    thread_ = std::thread([this, host, port]() {
        if (!svr_.listen(host, port)) {
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
    running_ = false;
}

}  // namespace remote_assist

