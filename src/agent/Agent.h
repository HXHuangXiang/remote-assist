#pragma once

#include "agent/Capture.h"
#include "agent/DesktopAccess.h"
#include "agent/EncoderMf.h"
#include "common/Config.h"
#include "net/HttpWsServer.h"

#include <atomic>
#include <memory>
#include <thread>

namespace remote_assist {

// Agent 模式主控:由 service 用 winlogon token 启动到当前桌面后,在此编排
// 桌面跟随 -> 采集 -> H.264 编码 -> WebSocket 广播,以及上行键鼠事件注入。
class Agent {
public:
    Agent() = default;
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // 阻塞运行直到 Stop()。返回进程退出码。
    int Run();
    void Stop();

private:
    void CaptureLoop();
    void OnMessage(const std::string& msg);
    std::string MakeCfgJson() const;
    static std::string WebDirFromExe();

    DesktopAccess desktop_;
    Capture capture_;
    std::unique_ptr<EncoderMf> encoder_;
    HttpWsServer server_;
    Config cfg_;
    std::atomic<bool> stop_{false};
    std::thread captureThread_;

    int deskWidth_ = 0;
    int deskHeight_ = 0;
    bool encoderReady_ = false;
};

}  // namespace remote_assist

