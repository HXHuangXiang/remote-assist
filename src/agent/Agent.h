#pragma once

#include <windows.h>

#include "agent/Capture.h"
#include "agent/EncoderMf.h"
#include "common/Config.h"
#include "net/HttpWsServer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace remote_assist {

// Agent 模式主控:由 service 用 winlogon token 启动到当前桌面后,在此编排
// 桌面跟随 -> 采集 -> JPEG 编码 -> WebSocket 广播,以及上行键鼠事件注入。
class Agent {
public:
    Agent() = default;
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // 阻塞运行直到 Stop()。返回进程退出码。
    int Run(bool serviceManaged = false);
    void Stop();

private:
    void CaptureLoop();
    void OnMessage(const std::string& msg);
    void OnControllerDisconnected();
    std::string MakeCfgJson() const;
    static std::string WebDirFromExe();

    Capture capture_;
    std::unique_ptr<EncoderMf> encoder_;
    HttpWsServer server_;
    Config cfg_;
    std::atomic<bool> stop_{false};
    std::thread captureThread_;
    HANDLE instanceMutex_ = nullptr;
    HANDLE stopEvent_ = nullptr;

    std::atomic<int> deskWidth_{0};
    std::atomic<int> deskHeight_{0};
    std::atomic<bool> frameResetRequested_{false};
    bool encoderReady_ = false;
    uint64_t previousFrameFingerprint_ = 0;
    std::chrono::steady_clock::time_point lastFrameSent_{};
    bool firstFrame_ = true;
};

}  // namespace remote_assist
