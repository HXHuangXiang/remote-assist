#pragma once

#include <windows.h>

#include "agent/Capture.h"
#include "agent/EncoderMf.h"
#include "common/Config.h"
#include "net/HttpWsServer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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
    // GDI 空闲路径可低频采集；离散远端输入到达时用该事件立即唤醒采集线程，避免
    // 为省 CPU 而把鼠标点击和密码输入的视觉反馈延后一个完整空闲帧周期。
    // requestFreshFrame 只用于能造成界面变化的离散输入（键盘、按键鼠标、滚轮）。
    // 纯鼠标移动由浏览器立即预测指针位置、采集路径按交互帧率校正。首个移动和
    // 后续受限频率的移动可唤醒采集，但不能因每个 mousemove 打断 BitBlt 等待并
    // 退化成浏览器刷新率的全帧复制。
    void WakeGdiCaptureLoop(bool requestFreshFrame = false);
    void WaitForGdiCaptureDelay(int delayMs) const;
    std::string MakeCfgJson() const;
    static std::string WebDirFromExe();

    Capture capture_;
    std::unique_ptr<EncoderMf> encoder_;
    HttpWsServer server_;
    Config cfg_;
    std::atomic<bool> stop_{false};
    // 最多可有少量 WebSocket 认证线程并发进入。密码升级会改写 cfg_ 中的 hash/salt，
    // 必须串行化，避免同一历史配置被重复迁移或出现 std::string 数据竞争。
    std::mutex passwordConfigMu_;
    std::thread captureThread_;
    HANDLE instanceMutex_ = nullptr;
    bool instanceMutexOwned_ = false;
    HANDLE stopEvent_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HANDLE frameReadyEvent_ = nullptr;
    HANDLE gdiCaptureWakeEvent_ = nullptr;

    std::atomic<int> deskWidth_{0};
    std::atomic<int> deskHeight_{0};
    std::atomic<bool> frameResetRequested_{false};
    // 锁屏/跨显卡等 GDI 路径每次都要同步读取桌面像素。输入线程记录最近合法远端
    // 操作，采集线程据此仅在交互窗口内保持较高采样率，静止时降低开销。
    std::atomic<uint64_t> lastRemoteInputTick_{0};
    // 连续鼠标移动只按 GDI 交互帧率唤醒采集，既让首次 hover/光标校正及时可见，
    // 也避免高刷新率浏览器把 GDI BitBlt 推到 60Hz 以上。
    std::atomic<uint64_t> lastGdiMoveWakeTick_{0};
    // 有效的离散远端输入要求下一次 GDI 采样跳过指纹去重。该标志只由输入回调
    // 写、采集线程读取；DXGI 路径仍沿用 Desktop Duplication 的变化通知。
    std::atomic<bool> gdiFreshFrameRequested_{false};
    // 断连回调由 WebSocket 工作线程触发；质量参数只能由采集线程操作，因此用该标志
    // 请求其在下一轮恢复用户配置上限。
    std::atomic<bool> qualityResetRequested_{false};
    // 断连恰逢输入桌面不可绑定时，不能把释放事件注入旧 desktop。该标志让已经
    // 成功绑定当前 desktop 的采集线程补做 ReleaseAll，避免修饰键或鼠标按键卡住。
    std::atomic<bool> inputReleaseRequested_{false};
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
    // WebCodecs 从收到压缩帧到输出 VideoFrame 的耗时。与 clientDraw* 总呈现
    // 耗时相减可判断卡顿来自解码器还是 requestAnimationFrame/Canvas。
    std::atomic<uint64_t> clientDecodeMsTotal_{0};
    std::atomic<uint64_t> clientDecodeMsSamples_{0};
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
