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
// 桌面跟随 -> 采集 -> H.264/JPEG 编码 -> WebSocket 广播,以及上行键鼠事件注入。
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
    // 每次编码尺寸变化时递增。控制端据此丢弃异步解码完成的旧尺寸画面。
    std::atomic<uint64_t> streamId_{0};
    // MakeCfgJson 可在 WebSocket 线程调用，不能直接读取 capture 线程持有的编码器。
    std::atomic<bool> streamH264_{false};
    // 从 SPS 提取的 avc1.PPCCLL，供 WebSocket 线程安全生成浏览器解码配置。
    std::atomic<uint32_t> streamH264Profile_{0x42E01E};
    bool encoderReady_ = false;
    // 连接重建、切屏和解码器恢复后，在看到真正的 H.264 IDR 前不发送增量帧。
    // 仅由采集线程访问。
    bool streamKeyFrameRequired_ = true;
    uint64_t previousFrameFingerprint_ = 0;
    std::chrono::steady_clock::time_point lastFrameSent_{};
    bool firstFrame_ = true;
};

}  // namespace remote_assist
