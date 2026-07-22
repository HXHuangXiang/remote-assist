#pragma once

#include <windows.h>

#include "agent/Capture.h"
#include "agent/EncoderMf.h"
#include "common/Config.h"
#include "net/HttpWsServer.h"

#include <atomic>
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
    HANDLE readyEvent_ = nullptr;

    std::atomic<int> deskWidth_{0};
    std::atomic<int> deskHeight_{0};
    std::atomic<bool> frameResetRequested_{false};
    // 锁屏/跨显卡等 GDI 路径每次采集都要完整 BitBlt 虚拟桌面。输入线程记录最近
    // 合法远端操作，采集线程据此仅在交互窗口内保持较高采样率，静止时降低开销。
    std::atomic<uint64_t> lastRemoteInputTick_{0};
    // 断连回调由 WebSocket 工作线程触发；质量参数只能由采集线程操作，因此用该标志
    // 请求其在下一轮恢复用户配置上限。
    std::atomic<bool> qualityResetRequested_{false};
    // 每次编码尺寸变化时递增。控制端据此丢弃异步解码完成的旧尺寸画面。
    std::atomic<uint64_t> streamId_{0};
    // MakeCfgJson 可在 WebSocket 线程调用，不能直接读取 capture 线程持有的编码器。
    std::atomic<bool> streamH264_{false};
    // 从 SPS 提取的 avc1.PPCCLL，供 WebSocket 线程安全生成浏览器解码配置。
    std::atomic<uint32_t> streamH264Profile_{0x42E01E};
    // cfg_.fps 是用户配置的上限。采集线程按浏览器 ACK 与 H.264 重同步状态动态下调，
    // 防止慢链路堆满两帧窗口后反复丢弃增量帧。
    std::atomic<int> streamFps_{30};
    // 浏览器每 5 秒上报一次呈现侧增量指标；CaptureLoop 用 exchange 取走并写入同一
    // 条 stream metrics，便于区分“编码慢”和“浏览器解码/绘制慢”。
    std::atomic<uint64_t> clientDrawnFrames_{0};
    std::atomic<uint64_t> clientDroppedFrames_{0};
    std::atomic<uint64_t> clientDrawMsTotal_{0};
    std::atomic<uint64_t> clientDrawMsSamples_{0};
    std::atomic<uint64_t> clientDecodeErrors_{0};
    std::atomic<uint32_t> clientMaxDecodeQueue_{0};
    std::atomic<uint32_t> clientMaxWsBufferedBytes_{0};
    bool encoderReady_ = false;
    // 连接重建、切屏和解码器恢复后，在看到真正的 H.264 IDR 前不发送增量帧。
    // 仅由采集线程访问。
    bool streamKeyFrameRequired_ = true;
    bool firstFrame_ = true;
};

}  // namespace remote_assist
