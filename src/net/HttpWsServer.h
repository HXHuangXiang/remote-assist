#pragma once

// 确保不引入 OpenSSL(我们的 WebSocket 走 HTTP 明文,#ifdef 会把 =0 也视为已定义)。
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace remote_assist {

// 维护当前已通过鉴权的 WebSocket 连接,供 agent 编码线程广播二进制 H.264 帧或文本 JSON。
class WsBroadcaster {
public:
    void Add(httplib::ws::WebSocket* ws);
    void Remove(httplib::ws::WebSocket* ws);
    void BroadcastBinary(const char* data, size_t len);
    void BroadcastText(const std::string& msg);
    int Count();

private:
    std::mutex mu_;
    std::vector<httplib::ws::WebSocket*> clients_;
};

// HTTP + WebSocket 服务。HTTP 挂载 web/ 静态页面;WebSocket /ws 承载鉴权、配置下发与键鼠事件。
class HttpWsServer {
public:
    using OnMessage = std::function<void(const std::string& msg)>;
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
    WsBroadcaster& Broadcaster() { return broadcaster_; }

    bool Start(const std::string& host, int port);
    void Stop();

private:
    httplib::Server svr_;
    WsBroadcaster broadcaster_;
    AuthVerifier authVerifier_;
    CfgProvider cfgProvider_;
    OnMessage onMessage_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace remote_assist
