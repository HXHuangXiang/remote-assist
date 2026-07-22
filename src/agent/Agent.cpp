#include "agent/Agent.h"

#include "agent/DesktopAccess.h"
#include "agent/Input.h"
#include "common/Log.h"
#include "common/RuntimeNames.h"

#include <nlohmann/json.hpp>

#include <objbase.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#pragma comment(lib, "ole32.lib")

namespace remote_assist {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                     static_cast<int>(w.size()), nullptr, 0,
                                     nullptr, nullptr);
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// 对缩放后的 BGRA 帧做等步长采样哈希。相比保存完整上一帧，避免每个 1080p
// 帧额外复制约 8MB；一秒一次的强制刷新兜底极小区域恰好未被采样的情况。
uint64_t FrameFingerprint(const std::vector<uint8_t>& frame) {
    constexpr size_t kStride = 64;
    uint64_t hash = 1469598103934665603ULL;
    for (size_t offset = 0; offset + sizeof(uint32_t) <= frame.size(); offset += kStride) {
        uint32_t pixel = 0;
        std::memcpy(&pixel, frame.data() + offset, sizeof(pixel));
        hash ^= pixel;
        hash *= 1099511628211ULL;
    }
    return hash ^ static_cast<uint64_t>(frame.size());
}

// 采集线程私有统计，按窗口输出增量而非每帧打日志，既便于定位性能瓶颈又不会
// 因日志 I/O 干扰远控体验。
struct CaptureLoopStats {
    uint64_t captureAttempts = 0;
    uint64_t capturedFrames = 0;
    uint64_t directMappedFrames = 0;
    uint64_t noChangeFrames = 0;
    uint64_t pointerOnlyFrames = 0;
    uint64_t captureFailures = 0;
    uint64_t fingerprintSkipped = 0;
    uint64_t encodeAttempts = 0;
    uint64_t encodedFrames = 0;
    uint64_t encodeFailures = 0;
    uint64_t encodedBytes = 0;
    uint64_t captureTimeMs = 0;
    uint64_t encodeTimeMs = 0;
};

// 输出上限按网络状态逐级调整。Capture 会始终保持原始宽高比，因此超宽屏、4:3
// 等非 16:9 桌面也不会被拉伸；实际尺寸仍通过 cfg 消息通知浏览器。
struct StreamResolutionCap {
    int width;
    int height;
};

constexpr StreamResolutionCap kStreamResolutionCaps[] = {
    {1920, 1080},
    {1600, 900},
    {1280, 720},
    {960, 540},
    {640, 360},
};
constexpr int kStreamResolutionCapCount =
    static_cast<int>(sizeof(kStreamResolutionCaps) / sizeof(kStreamResolutionCaps[0]));

// 直接映射的 DXGI staging texture 只在“本轮采集 -> 本轮编码”之间有效。这个 guard
// 保证初始化失败、异常返回或未来新增的 continue 分支都不会遗留 Map 状态。
class CapturedFrameLease {
public:
    CapturedFrameLease(Capture& capture, CapturedFrame& frame)
        : capture_(capture), frame_(frame) {}
    ~CapturedFrameLease() { Release(); }

    CapturedFrameLease(const CapturedFrameLease&) = delete;
    CapturedFrameLease& operator=(const CapturedFrameLease&) = delete;

    void Release() {
        if (active_) {
            capture_.ReleaseFrame(frame_);
            active_ = false;
        }
    }

private:
    Capture& capture_;
    CapturedFrame& frame_;
    bool active_ = true;
};

uint64_t CounterDelta(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : 0;
}

// C++17 没有 atomic::fetch_max。浏览器指标只用于诊断，保留一个窗口内峰值即可，
// 通过 CAS 避免高频输入回调为此引入额外 mutex。
void UpdateAtomicMax(std::atomic<uint32_t>& target, uint32_t value) {
    uint32_t current = target.load(std::memory_order_relaxed);
    while (current < value && !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

std::string AvcCodecString(uint32_t profile) {
    char codec[16] = {};
    std::snprintf(codec, sizeof(codec), "avc1.%06X", profile & 0xFFFFFF);
    return codec;
}

bool ReadJsonBool(const nlohmann::json& object, const char* key, bool& value) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) {
        return false;
    }
    value = it->get<bool>();
    return true;
}

bool ReadOptionalJsonBool(const nlohmann::json& object, const char* key, bool& value) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        return false;
    }
    value = it->get<bool>();
    return true;
}

bool ReadJsonIntInRange(const nlohmann::json& object, const char* key, int min,
                        int max, int& value) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return false;
    }
    if (it->is_number_unsigned()) {
        const uint64_t number = it->get<uint64_t>();
        if (number > static_cast<uint64_t>(max)) {
            return false;
        }
        value = static_cast<int>(number);
        return value >= min;
    }
    if (!it->is_number_integer()) {
        return false;
    }
    const int64_t number = it->get<int64_t>();
    if (number < min || number > max) {
        return false;
    }
    value = static_cast<int>(number);
    return true;
}

bool ReadJsonFiniteNumber(const nlohmann::json& object, const char* key, double& value) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return false;
    }
    value = it->get<double>();
    return std::isfinite(value);
}

bool ReadJsonString(const nlohmann::json& object, const char* key, std::string& value) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return false;
    }
    value = it->get<std::string>();
    return true;
}

}  // namespace

Agent::~Agent() {
    Stop();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    if (readyEvent_) {
        CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
    }
    if (instanceMutex_) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
}

std::string Agent::WebDirFromExe() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe = buf;
    const size_t pos = exe.find_last_of(L"\\/");
    std::wstring dir = (pos != std::wstring::npos) ? exe.substr(0, pos) : exe;
    dir += L"\\web";
    if (!PathFileExistsW(dir.c_str())) {
        // 兼容早期 CMake install 目录：exe 位于 bin/，网页在 prefix/share/。
        // 当前安装布局已统一为 exe 同级 web/，此分支仅用于平滑升级旧安装。
        std::wstring alt = dir + L"\\..\\..\\share\\remote-assist\\web";
        wchar_t full[MAX_PATH] = {};
        if (GetFullPathNameW(alt.c_str(), MAX_PATH, full, nullptr)) {
            if (PathFileExistsW(full)) {
                return WideToUtf8(full);
            }
        }
    }
    return WideToUtf8(dir);
}

int Agent::Run(bool serviceManaged) {
    log::Init(LogDir(), L"agent.log");
    log::Info("agent starting");

    // 保持 mutex 的初始所有权，使服务能够可靠地区分“名称仍存在”与“已有
    // Agent 正在运行”。所有权会在本线程/进程退出时由系统自动释放。
    instanceMutex_ = CreateMutexW(nullptr, TRUE, runtime::kAgentMutexName);
    const DWORD mutexError = GetLastError();
    if (!instanceMutex_) {
        log::Error("agent mutex creation failed: " + std::to_string(mutexError));
        return 1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {
        log::Warn("agent already running, skip duplicate launch");
        return 0;
    }

    if (serviceManaged) {
        stopEvent_ = OpenEventW(SYNCHRONIZE, FALSE, runtime::kAgentStopEventName);
        if (!stopEvent_) {
            log::Error("agent stop event open failed: " + std::to_string(GetLastError()));
            return 1;
        }
        readyEvent_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, runtime::kAgentReadyEventName);
        if (!readyEvent_) {
            // 就绪状态仅用于诊断；服务停止事件仍是 Agent 的必备生命周期契约。
            log::Warn("agent ready event open failed: " + std::to_string(GetLastError()));
        }
    } else {
        // 独立运行的 Agent 也监听同一停止事件。服务安装/启动时可先通知它
        // 退出并接管，避免两个实例互相争夺端口与全局 mutex。
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, runtime::kAgentStopEventName);
        if (!stopEvent_) {
            log::Error("agent stop event creation failed: " + std::to_string(GetLastError()));
            return 1;
        }
        ResetEvent(stopEvent_);
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);


    cfg_ = LoadOrCreateConfig();
    if (cfg_.passwordHash.empty() || cfg_.salt.empty()) {
        log::Error("agent: valid configuration unavailable");
        CoUninitialize();
        return 1;
    }
    Input::Enable();
    log::Info("agent config port=" + std::to_string(cfg_.port) +
              " fps=" + std::to_string(cfg_.fps));
    streamFps_.store(cfg_.fps);

    // 显示器清单在启动网络服务前完成,后续仅由采集线程读取。
    capture_.EnumMonitors();

    const std::string webDir = WebDirFromExe();
    if (!server_.SetWebDir(webDir)) {
        log::Error("agent: web resource directory unavailable: " + webDir);
        CoUninitialize();
        return 1;
    }
    server_.SetAuthVerifier([this](const std::string& token) {
        return VerifyPassword(cfg_, token);
    });
    server_.SetCfgProvider([this]() { return MakeCfgJson(); });
    server_.SetOnMessage([this](const std::string& msg) { OnMessage(msg); });
    server_.SetOnControllerDisconnected([this]() { OnControllerDisconnected(); });

    if (!server_.Start("0.0.0.0", cfg_.port)) {
        log::Error("agent: server start failed");
        CoUninitialize();
        return 1;
    }
    if (readyEvent_) {
        SetEvent(readyEvent_);
    }

    captureThread_ = std::thread(&Agent::CaptureLoop, this);

    // 主线程等待停止信号，同时监控 HTTP/WS accept 循环。监听线程意外退出时不能继续
    // 保留“Agent 已就绪”状态，否则服务和配置界面会误报控制端可连接。
    int runResult = 0;
    while (!stop_.load()) {
        const DWORD waitResult = stopEvent_
            ? WaitForSingleObject(stopEvent_, 200)
            : WAIT_TIMEOUT;
        if (waitResult == WAIT_OBJECT_0) {
            log::Info("agent stop event received");
            Stop();
        } else if (!server_.IsRunning()) {
            log::Error("agent: HTTP/WS listener stopped unexpectedly");
            runResult = 1;
            Stop();
        }
    }

    Stop();
    CoUninitialize();
    log::Info("agent exit");
    return runResult;
}

void Agent::Stop() {
    if (stop_.exchange(true)) {
        return;
    }
    if (readyEvent_) {
        ResetEvent(readyEvent_);
    }
    // 先拒绝在途 WebSocket 回调的新输入，再释放已按下状态。主线程临时绑定当前输入桌面，
    // 尽早释放控制端遗留的修饰键和鼠标按键，再等待 WebSocket 工作线程退出。
    Input::Disable();
    DesktopAccess shutdownDesktop;
    if (shutdownDesktop.Bind()) {
        Input::ReleaseAll();
    } else {
        log::Warn("agent stop: unable to bind input desktop for input release");
    }
    server_.Stop();
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (encoder_) {
        encoder_->Release();
        encoder_.reset();
    }
}

void Agent::CaptureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DesktopAccess captureDesktop;
    if (!captureDesktop.Bind()) {
        log::Error("capture thread: bind desktop failed");
    }
    if (!capture_.Init()) {
        log::Error("capture thread: capture init failed");
    }
    auto nextDesktopCheck = std::chrono::steady_clock::now();
    auto metricsStartedAt = nextDesktopCheck;
    auto nextMetricsLog = nextDesktopCheck + std::chrono::seconds(10);
    auto nextAdaptation = nextDesktopCheck + std::chrono::seconds(1);
    CaptureLoopStats metrics;
    BroadcasterStats broadcasterStatsBase = server_.Broadcaster().SnapshotStats();
    BroadcasterStats adaptationStatsBase = broadcasterStatsBase;
    int adaptiveFps = cfg_.fps;
    int adaptiveBitrate = cfg_.bitrate;
    bool adaptiveBitrateAvailable = true;
    int adaptiveResolutionTier = 0;
    int congestionWindows = 0;
    int healthyResolutionWindows = 0;
    // 采集结果在相邻帧间复用容量。1920x1080 的 BGRA 帧约 8MB，若每轮都创建
    // 局部 CapturedFrame 会触发持续的大块堆分配和释放，直接放大卡顿。
    CapturedFrame frame;

    auto setResolutionTier = [&](int tier) {
        tier = std::clamp(tier, 0, kStreamResolutionCapCount - 1);
        if (tier == adaptiveResolutionTier) {
            return;
        }
        const StreamResolutionCap previous = kStreamResolutionCaps[adaptiveResolutionTier];
        adaptiveResolutionTier = tier;
        const StreamResolutionCap current = kStreamResolutionCaps[adaptiveResolutionTier];
        capture_.SetMaxOutputSize(current.width, current.height);
        log::Info("adaptive stream cap " + std::to_string(previous.width) + "x" +
                  std::to_string(previous.height) + " -> " +
                  std::to_string(current.width) + "x" + std::to_string(current.height));
    };

    // 自适应码率是按“当前控制端”的链路质量得出的。控制端断开后若直接保留低码率，
    // 下一台网络正常的电脑会长时间看到低质量画面；空闲期重置不影响正在播放的流。
    auto restoreBitrateForNextController = [&] {
        if (adaptiveBitrate == cfg_.bitrate) {
            return;
        }
        const int previousBitrate = adaptiveBitrate;
        bool reinitializeEncoder = false;
        if (encoder_ && encoder_->IsH264()) {
            if (!adaptiveBitrateAvailable || !encoder_->UpdateBitrate(cfg_.bitrate)) {
                // 当前 MFT 无法把运行期码率升回配置上限时，在没有控制端的安全窗口
                // 丢弃它；下一张画面会用配置码率重建编码器。
                reinitializeEncoder = true;
                adaptiveBitrateAvailable = true;
            }
        }
        adaptiveBitrate = cfg_.bitrate;
        if (reinitializeEncoder) {
            encoder_->Release();
            encoder_.reset();
            encoderReady_ = false;
            log::Info("adaptive bitrate reset will reinitialize H.264 encoder");
        }
        log::Info("adaptive stream bitrate reset " + std::to_string(previousBitrate) + " -> " +
                  std::to_string(adaptiveBitrate));
    };

    auto restoreQualityForNextController = [&] {
        if (adaptiveFps != cfg_.fps) {
            adaptiveFps = cfg_.fps;
            streamFps_.store(adaptiveFps);
        }
        restoreBitrateForNextController();
        if (adaptiveResolutionTier != 0) {
            setResolutionTier(0);
        }
        congestionWindows = 0;
        healthyResolutionWindows = 0;
    };

    auto logMetricsIfDue = [&](std::chrono::steady_clock::time_point now) {
        if (now < nextMetricsLog) {
            return;
        }
        const BroadcasterStats broadcasterStats = server_.Broadcaster().SnapshotStats();
        const uint64_t queued = CounterDelta(broadcasterStats.queuedFrames,
                                             broadcasterStatsBase.queuedFrames);
        const uint64_t replaced = CounterDelta(broadcasterStats.replacedFrames,
                                               broadcasterStatsBase.replacedFrames);
        const uint64_t sent = CounterDelta(broadcasterStats.sentFrames,
                                           broadcasterStatsBase.sentFrames);
        const uint64_t sentBytes = CounterDelta(broadcasterStats.sentBytes,
                                                broadcasterStatsBase.sentBytes);
        const uint64_t acknowledged = CounterDelta(broadcasterStats.acknowledgedFrames,
                                                   broadcasterStatsBase.acknowledgedFrames);
        const uint64_t ackLatencyUs = CounterDelta(broadcasterStats.ackLatencyUs,
                                                   broadcasterStatsBase.ackLatencyUs);
        const uint64_t ackLatencySamples = CounterDelta(broadcasterStats.ackLatencySamples,
                                                        broadcasterStatsBase.ackLatencySamples);
        const uint64_t ackTimeouts = CounterDelta(broadcasterStats.ackTimeouts,
                                                  broadcasterStatsBase.ackTimeouts);
        const uint64_t sendFailures = CounterDelta(broadcasterStats.sendFailures,
                                                   broadcasterStatsBase.sendFailures);
        const uint64_t h264Resyncs = CounterDelta(broadcasterStats.h264Resyncs,
                                                  broadcasterStatsBase.h264Resyncs);
        const uint64_t clientDrawn = clientDrawnFrames_.exchange(0, std::memory_order_relaxed);
        const uint64_t clientDropped = clientDroppedFrames_.exchange(0, std::memory_order_relaxed);
        const uint64_t clientDrawMsTotal =
            clientDrawMsTotal_.exchange(0, std::memory_order_relaxed);
        const uint64_t clientDrawMsSamples =
            clientDrawMsSamples_.exchange(0, std::memory_order_relaxed);
        const uint64_t clientDecodeErrors =
            clientDecodeErrors_.exchange(0, std::memory_order_relaxed);
        const uint32_t clientMaxDecodeQueue =
            clientMaxDecodeQueue_.exchange(0, std::memory_order_relaxed);
        const uint32_t clientMaxWsBuffered =
            clientMaxWsBufferedBytes_.exchange(0, std::memory_order_relaxed);
        const bool active = metrics.captureAttempts != 0 || queued != 0 || sent != 0 ||
            acknowledged != 0 || ackTimeouts != 0 || sendFailures != 0 || h264Resyncs != 0 ||
            clientDrawn != 0 || clientDropped != 0 || clientDecodeErrors != 0;
        if (active) {
            const auto elapsedMs = std::max<int64_t>(1,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - metricsStartedAt).count());
            const uint64_t avgCaptureMs = metrics.captureAttempts == 0 ? 0 :
                metrics.captureTimeMs / metrics.captureAttempts;
            const uint64_t avgEncodeMs = metrics.encodeAttempts == 0 ? 0 :
                metrics.encodeTimeMs / metrics.encodeAttempts;
            const uint64_t avgAckMs = ackLatencySamples == 0 ? 0 :
                ackLatencyUs / ackLatencySamples / 1000;
            const uint64_t clientDrawAvgMs = clientDrawMsSamples == 0 ? 0 :
                clientDrawMsTotal / clientDrawMsSamples;
            log::Info("stream metrics " + std::to_string(elapsedMs) + "ms clients=" +
                      std::to_string(server_.Broadcaster().Count()) +
                      " capture=" + std::to_string(metrics.capturedFrames) + "/" +
                      std::to_string(metrics.captureAttempts) +
                      " direct=" + std::to_string(metrics.directMappedFrames) +
                      " unchanged=" + std::to_string(metrics.noChangeFrames) +
                      " pointer_only=" + std::to_string(metrics.pointerOnlyFrames) +
                      " sampled_skip=" + std::to_string(metrics.fingerprintSkipped) +
                      " capture_fail=" + std::to_string(metrics.captureFailures) +
                      " capture_avg_ms=" + std::to_string(avgCaptureMs) +
                      " encode=" + std::to_string(metrics.encodedFrames) + "/" +
                      std::to_string(metrics.encodeAttempts) +
                      " encode_fail=" + std::to_string(metrics.encodeFailures) +
                      " encode_avg_ms=" + std::to_string(avgEncodeMs) +
                      " encoded_kib=" + std::to_string(metrics.encodedBytes / 1024) +
                      " queued=" + std::to_string(queued) +
                      " replaced=" + std::to_string(replaced) +
                      " sent=" + std::to_string(sent) +
                      " sent_kib=" + std::to_string(sentBytes / 1024) +
                      " ack=" + std::to_string(acknowledged) +
                      " ack_avg_ms=" + std::to_string(avgAckMs) +
                      " ack_timeout=" + std::to_string(ackTimeouts) +
                      " send_fail=" + std::to_string(sendFailures) +
                      " h264_resync=" + std::to_string(h264Resyncs) +
                      " client_drawn=" + std::to_string(clientDrawn) +
                      " client_dropped=" + std::to_string(clientDropped) +
                      " client_draw_avg_ms=" + std::to_string(clientDrawAvgMs) +
                      " client_decode_errors=" + std::to_string(clientDecodeErrors) +
                      " client_decode_queue_max=" + std::to_string(clientMaxDecodeQueue) +
                      " client_ws_buffered_max=" + std::to_string(clientMaxWsBuffered) +
                      " stream_fps=" + std::to_string(adaptiveFps) +
                      " stream_bitrate=" + std::to_string(adaptiveBitrate) +
                      " stream_cap=" +
                      std::to_string(kStreamResolutionCaps[adaptiveResolutionTier].width) + "x" +
                      std::to_string(kStreamResolutionCaps[adaptiveResolutionTier].height));
        }
        metrics = {};
        broadcasterStatsBase = broadcasterStats;
        metricsStartedAt = now;
        nextMetricsLog = now + std::chrono::seconds(10);
    };

    // 发送队列只能安全保留很少的 H.264 增量帧；一旦浏览器绘制确认追不上，继续
    // 以固定 FPS 编码只会反复触发 IDR 重同步。依据一秒窗口的 ACK 端到端延迟
    // 快速降帧，网络恢复后再平滑回升到用户配置上限。
    auto adaptFrameRateIfDue = [&](std::chrono::steady_clock::time_point now) {
        if (now < nextAdaptation) {
            return;
        }
        const BroadcasterStats stats = server_.Broadcaster().SnapshotStats();
        const uint64_t ackLatencyUs = CounterDelta(stats.ackLatencyUs,
                                                   adaptationStatsBase.ackLatencyUs);
        const uint64_t ackSamples = CounterDelta(stats.ackLatencySamples,
                                                 adaptationStatsBase.ackLatencySamples);
        const uint64_t ackTimeouts = CounterDelta(stats.ackTimeouts,
                                                  adaptationStatsBase.ackTimeouts);
        const uint64_t h264Resyncs = CounterDelta(stats.h264Resyncs,
                                                  adaptationStatsBase.h264Resyncs);
        const uint64_t sendFailures = CounterDelta(stats.sendFailures,
                                                   adaptationStatsBase.sendFailures);
        const uint64_t averageAckMs = ackSamples == 0 ? 0 : ackLatencyUs / ackSamples / 1000;
        const bool congested = ackTimeouts != 0 || h264Resyncs != 0 || sendFailures != 0 ||
            (ackSamples != 0 && averageAckMs >= 120);
        const bool healthy = ackSamples >= 4 && averageAckMs <= 65 && ackTimeouts == 0 &&
            h264Resyncs == 0 && sendFailures == 0;
        const bool severeCongestion = ackTimeouts != 0 || h264Resyncs != 0 ||
            sendFailures != 0;
        int nextFps = adaptiveFps;
        int nextBitrate = adaptiveBitrate;
        const int minimumBitrate = std::min(cfg_.bitrate,
            std::max(100'000, cfg_.bitrate / 8));
        if (congested) {
            // 乘法退避能在高延迟 Wi-Fi 下更快逃离“P 帧挤压 -> IDR”的循环。
            nextFps = std::max(5, adaptiveFps * 3 / 4);
            nextBitrate = std::max(minimumBitrate, adaptiveBitrate * 3 / 4);
        } else if (healthy) {
            // 恢复阶段使用小步上调，避免刚恢复就再次压垮解码/网络窗口。
            nextFps = std::min(cfg_.fps, adaptiveFps + 2);
            nextBitrate = std::min(cfg_.bitrate,
                adaptiveBitrate + std::max(100'000, cfg_.bitrate / 20));
        }

        if (congested) {
            healthyResolutionWindows = 0;
            ++congestionWindows;
            // 帧率与码率先立即退避；持续两秒或发生超时/重同步时才切换分辨率，
            // 避免短暂的浏览器卡顿反复重建 H.264 MFT。
            if ((severeCongestion || congestionWindows >= 2) &&
                adaptiveResolutionTier + 1 < kStreamResolutionCapCount) {
                setResolutionTier(adaptiveResolutionTier + 1);
                congestionWindows = 0;
            }
        } else if (healthy) {
            congestionWindows = 0;
            ++healthyResolutionWindows;
            // 恢复分辨率比恢复 FPS/码率更保守，连续健康五秒才上调一档。
            if (healthyResolutionWindows >= 5 && adaptiveResolutionTier > 0) {
                setResolutionTier(adaptiveResolutionTier - 1);
                healthyResolutionWindows = 0;
            }
        } else {
            congestionWindows = 0;
            healthyResolutionWindows = 0;
        }

        if (nextFps != adaptiveFps) {
            log::Info("adaptive stream fps " + std::to_string(adaptiveFps) + " -> " +
                      std::to_string(nextFps) + " ack_avg_ms=" +
                      std::to_string(averageAckMs) + " ack_timeout=" +
                      std::to_string(ackTimeouts) + " h264_resync=" +
                      std::to_string(h264Resyncs));
            adaptiveFps = nextFps;
            streamFps_.store(adaptiveFps);
        }
        if (nextBitrate != adaptiveBitrate && adaptiveBitrateAvailable && encoder_ &&
            encoder_->IsH264()) {
            if (encoder_->UpdateBitrate(nextBitrate)) {
                adaptiveBitrate = nextBitrate;
            } else {
                // 部分 MFT 只能在 SetOutputType 前写入码率。只记录一次，后续仍由
                // FPS 自适应保持低延迟，避免每秒重复失败和刷日志。
                adaptiveBitrateAvailable = false;
                log::Info("H.264 MFT does not support dynamic bitrate; using FPS adaptation only");
            }
        }
        adaptationStatsBase = stats;
        nextAdaptation = now + std::chrono::seconds(1);
    };

    while (!stop_.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        logMetricsIfDue(t0);
        adaptFrameRateIfDue(t0);

        // JPEG 回退需要 CPU 全帧压缩，大屏时保守限帧；H.264 模式的压缩由 MFT
        // 承担，不应仅因分辨率达到 1080p 就被无条件锁在 15 FPS，否则既浪费
        // 硬件能力，也会让编码器的 30 FPS 时间戳和实际采集节奏不一致。
        int effectiveFps = adaptiveFps;
        if (capture_.IsUsingGdi()) {
            // BitBlt 即使画面未变也需要整帧复制到 DIB；锁屏和“全部屏幕”路径
            // 优先保持低延迟与输入响应，15 FPS 已足够避免远端界面出现明显跳变。
            effectiveFps = std::min(effectiveFps, 15);
        } else if ((!encoder_ || !encoder_->IsH264()) &&
                   (deskWidth_.load() >= 1920 || deskHeight_.load() >= 1080)) {
            effectiveFps = std::min(effectiveFps, 15);
        }
        const int targetMs = 1000 / std::max(1, effectiveFps);

        // 跟随桌面切换(锁屏↔解锁、Winlogon↔Default)。
        // 桌面与显示器布局检查是较重的系统调用,每秒一次即可覆盖锁屏切换、热插拔
        // 和分辨率调整。布局变化即使输出尺寸未变，也必须重新下发 monitor 列表。
        if (t0 >= nextDesktopCheck) {
            nextDesktopCheck = t0 + std::chrono::seconds(1);
            const bool desktopChanged = captureDesktop.CheckRebind();
            const bool displayChanged = capture_.EnumMonitors();
            if (desktopChanged || displayChanged) {
                capture_.ResetForDesktop();
                firstFrame_ = true;
                streamKeyFrameRequired_ = true;
                lastFrameSent_ = {};
                if (displayChanged) {
                    server_.Broadcaster().BroadcastText(MakeCfgJson());
                }
            }
        }

        // 没有控制端时不做抓屏、缩放和 JPEG 编码，既避免空转占用 CPU，
        // 也在下一次客户端连接时强制重新输出首帧。
        if (server_.Broadcaster().Count() == 0) {
            if (!firstFrame_) {
                firstFrame_ = true;
            }
            // 编码器仍会保留上一控制端的参考帧；新控制端绝不能从 delta 帧接续。
            streamKeyFrameRequired_ = true;
            lastFrameSent_ = {};
            restoreQualityForNextController();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // 控制端可能在 CaptureLoop 看到“零客户端”前就快速重连；断连回调仍会请求
        // 在采集线程安全地恢复上一位控制者留下的自适应质量状态。
        if (qualityResetRequested_.exchange(false)) {
            restoreQualityForNextController();
        }
        if (frameResetRequested_.exchange(false)) {
            firstFrame_ = true;
            streamKeyFrameRequired_ = true;
            lastFrameSent_ = {};
        }

        // DXGI 空闲时会阻塞等待桌面变化。等待一个目标帧周期即可在静态画面
        // 下避免轮询空转，同时不会再因固定 50ms 把 30FPS 限制为约 20FPS。
        const auto captureStartedAt = std::chrono::steady_clock::now();
        const CaptureResult captureResult = capture_.CaptureFrame(
            frame, static_cast<DWORD>(std::max(1, targetMs)));
        metrics.captureAttempts++;
        metrics.captureTimeMs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - captureStartedAt).count());
        PointerUpdate pointer;
        if (capture_.TakePointerUpdate(pointer)) {
            // 指针使用独立小消息发送。DXGI 的 pointer-only 通知不再触发整帧 H.264
            // 编码，浏览器仍能立即看到远端鼠标位置。
            nlohmann::json cursor = {
                {"t", "cursor"},
                {"visible", pointer.visible},
            };
            if (pointer.visible) {
                cursor["x"] = pointer.x;
                cursor["y"] = pointer.y;
            }
            server_.Broadcaster().BroadcastText(cursor.dump());
        }
        if (captureResult == CaptureResult::kNoChange ||
            captureResult == CaptureResult::kPointerOnly) {
            if (captureResult == CaptureResult::kPointerOnly) {
                metrics.pointerOnlyFrames++;
            } else {
                metrics.noChangeFrames++;
            }
            const auto t1 = std::chrono::steady_clock::now();
            const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            const int remain = targetMs - static_cast<int>(used);
            if (remain > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(remain));
            }
            continue;
        }
        if (captureResult == CaptureResult::kFailed) {
            metrics.captureFailures++;
            static int failCount = 0;
            if (++failCount % 300 == 1) {  // 每 10s(30fps*300)打一次,避免刷屏
                log::Warn("capture frame failed, count=" + std::to_string(failCount));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
            continue;
        }
        metrics.capturedFrames++;
        if (frame.IsDirectDxgi()) {
            metrics.directMappedFrames++;
        }
        CapturedFrameLease frameLease(capture_, frame);

        const bool needsFreshFrame = firstFrame_;
        if (!frame.IsDirectDxgi()) {
            const uint64_t frameFingerprint = FrameFingerprint(frame.data);
            const bool refreshDue = lastFrameSent_.time_since_epoch().count() == 0 ||
                t0 - lastFrameSent_ >= std::chrono::seconds(1);
            if (!firstFrame_ && !streamKeyFrameRequired_ &&
                frameFingerprint == previousFrameFingerprint_ && !refreshDue) {
                metrics.fingerprintSkipped++;
                frameLease.Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
                continue;
            }
            previousFrameFingerprint_ = frameFingerprint;
        }
        firstFrame_ = false;

        if (!encoder_ || frame.width != deskWidth_.load() || frame.height != deskHeight_.load()) {
            deskWidth_.store(frame.width);
            deskHeight_.store(frame.height);
            encoderReady_ = false;
            encoder_ = std::make_unique<EncoderMf>();
            if (!encoder_->Init(frame.width, frame.height, cfg_.fps, adaptiveBitrate)) {
                log::Error("encoder init failed for " + std::to_string(frame.width) +
                           "x" + std::to_string(frame.height));
                encoder_.reset();
                frameLease.Release();
                std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
                continue;
            }
            encoderReady_ = true;
            streamH264_.store(encoder_->IsH264());
            streamH264Profile_.store(encoder_->H264CodecProfile());
            streamKeyFrameRequired_ = encoder_->IsH264();
            // 尺寸/编码器变化后提升流版本并重新下发 cfg。浏览器异步图片解码可能
            // 在 cfg 到达后才完成，因此二进制帧会带上该版本供控制端判定。
            streamId_.fetch_add(1);
            server_.Broadcaster().BroadcastText(MakeCfgJson());
        }

        if (encoderReady_ && !frame.Empty()) {
            std::vector<EncodedChunk> chunks;
            const auto encodeStartedAt = std::chrono::steady_clock::now();
            const bool wasH264 = streamH264_.load();
            if (needsFreshFrame && encoder_->IsH264()) {
                encoder_->RequestKeyFrame();
            }
            const bool encoded = encoder_->Encode(frame.Pixels(), frame.StrideBytes(), chunks);
            metrics.encodeAttempts++;
            metrics.encodeTimeMs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - encodeStartedAt).count());
            if (encoded) {
                if (wasH264 != encoder_->IsH264()) {
                    streamH264_.store(encoder_->IsH264());
                    streamH264Profile_.store(encoder_->H264CodecProfile());
                    streamKeyFrameRequired_ = encoder_->IsH264();
                    streamId_.fetch_add(1);
                    server_.Broadcaster().BroadcastText(MakeCfgJson());
                }
                if (encoder_->IsH264()) {
                    const uint32_t actualProfile = encoder_->H264CodecProfile();
                    if (streamH264Profile_.load() != actualProfile) {
                        streamH264Profile_.store(actualProfile);
                        streamId_.fetch_add(1);
                        streamKeyFrameRequired_ = true;
                        const bool hasIdr = std::any_of(chunks.begin(), chunks.end(),
                            [](const EncodedChunk& chunk) { return chunk.isKey; });
                        if (!hasIdr) {
                            encoder_->RequestKeyFrame();
                        }
                        server_.Broadcaster().BroadcastText(MakeCfgJson());
                    }
                }
                bool sent = false;
                for (auto& c : chunks) {
                    if (c.data.empty()) {
                        continue;
                    }
                    if (encoder_->IsH264() && streamKeyFrameRequired_) {
                        if (!c.isKey) {
                            continue;
                        }
                        streamKeyFrameRequired_ = false;
                    }
                    metrics.encodedFrames++;
                    metrics.encodedBytes += c.data.size();
                    const bool h264 = encoder_->IsH264();
                    const FrameQueueResult queueResult =
                        server_.Broadcaster().BroadcastBinary(std::move(c.data), streamId_.load(),
                                                              c.isKey, c.timestampUs, h264);
                    if (queueResult == FrameQueueResult::kH264ResyncRequired) {
                        // 待发送 delta 已无法与下一帧构成连续 GOP；必须从新的
                        // IDR 恢复，不能沿用 JPEG 的“最新帧覆盖”策略。
                        streamKeyFrameRequired_ = true;
                        encoder_->RequestKeyFrame();
                        log::Warn("H.264 pending delta dropped, requesting IDR resync");
                        // 本轮若此前仅入队了待发送帧，它已经被清空，不能把它
                        // 计作已输出的新画面。
                        sent = false;
                        break;
                    }
                    sent = queueResult == FrameQueueResult::kQueued ||
                        queueResult == FrameQueueResult::kReplaced;
                }
                if (sent) {
                    lastFrameSent_ = std::chrono::steady_clock::now();
                }
            } else {
                metrics.encodeFailures++;
            }
        }

        // 不要让 staging texture 在帧率节流 sleep 期间保持 Map，下一轮可立即复用。
        frameLease.Release();
        const auto t1 = std::chrono::steady_clock::now();
        const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const int remain = targetMs - static_cast<int>(used);
        if (remain > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remain));
        }
    }
    CoUninitialize();
}

std::string Agent::MakeCfgJson() const {
    nlohmann::json j;
    j["t"] = "cfg";
    const bool h264 = streamH264_.load();
    j["codec"] = h264 ? AvcCodecString(streamH264Profile_.load()) : "jpeg";
    if (h264) {
        j["annexb"] = true;
    }
    j["w"] = deskWidth_.load();
    j["h"] = deskHeight_.load();
    j["fps"] = streamFps_.load();
    j["stream_id"] = streamId_.load();
    auto mons = nlohmann::json::array();
    for (const auto& m : capture_.MonitorsSnapshot()) {
        mons.push_back({{"index", m.index}, {"name", m.name}, {"w", m.w}, {"h", m.h}});
    }
    j["monitors"] = mons;
    j["selected_monitor"] = capture_.SelectedMonitor();
    return j.dump();
}

void Agent::OnMessage(const std::string& msg) {
    if (stop_.load()) {
        return;
    }
    const auto j = nlohmann::json::parse(msg, nullptr, false);
    if (!j.is_object()) {
        return;
    }
    std::string t;
    if (!ReadJsonString(j, "t", t)) {
        return;
    }

    if (t == "client_stats") {
        // 这些指标仅用于定位浏览器解码/绘制是否成为瓶颈。每个字段都限制为一个
        // 短统计窗口内的合理上限，避免异常页面把无界数值带入诊断日志。
        int drawn = 0;
        int dropped = 0;
        int drawMsTotal = 0;
        int drawMsSamples = 0;
        int decodeErrors = 0;
        int maxDecodeQueue = 0;
        int maxWsBuffered = 0;
        if (!ReadJsonIntInRange(j, "drawn", 0, 10'000, drawn) ||
            !ReadJsonIntInRange(j, "dropped", 0, 10'000, dropped) ||
            !ReadJsonIntInRange(j, "draw_ms_total", 0, 10'000'000, drawMsTotal) ||
            !ReadJsonIntInRange(j, "draw_ms_samples", 0, 10'000, drawMsSamples) ||
            !ReadJsonIntInRange(j, "decode_errors", 0, 10'000, decodeErrors) ||
            !ReadJsonIntInRange(j, "max_decode_queue", 0, 10'000, maxDecodeQueue) ||
            !ReadJsonIntInRange(j, "max_ws_buffered", 0, 16 * 1024 * 1024,
                                maxWsBuffered)) {
            return;
        }
        clientDrawnFrames_.fetch_add(static_cast<uint64_t>(drawn), std::memory_order_relaxed);
        clientDroppedFrames_.fetch_add(static_cast<uint64_t>(dropped),
                                       std::memory_order_relaxed);
        clientDrawMsTotal_.fetch_add(static_cast<uint64_t>(drawMsTotal),
                                     std::memory_order_relaxed);
        clientDrawMsSamples_.fetch_add(static_cast<uint64_t>(drawMsSamples),
                                       std::memory_order_relaxed);
        clientDecodeErrors_.fetch_add(static_cast<uint64_t>(decodeErrors),
                                      std::memory_order_relaxed);
        UpdateAtomicMax(clientMaxDecodeQueue_, static_cast<uint32_t>(maxDecodeQueue));
        UpdateAtomicMax(clientMaxWsBufferedBytes_, static_cast<uint32_t>(maxWsBuffered));
        return;
    }

    if (t == "keyframe") {
        // 浏览器在 WebCodecs 状态丢失后请求从独立可解码的 IDR 重新开始。
        frameResetRequested_.store(true);
        return;
    }
    if (t == "monitor") {
        int index = -1;
        if (!ReadJsonIntInRange(j, "index", -1, 1024, index)) {
            return;
        }
        capture_.SetMonitor(index);
        frameResetRequested_.store(true);
        log::Info("monitor switched to idx=" + std::to_string(capture_.SelectedMonitor()));
        return;
    }
    if (t != "key" && t != "mouse" && t != "move" && t != "wheel") {
        return;
    }

    // WebSocket 回调在线程池工作线程中执行。每个线程独立绑定当前输入桌面，
    // 避免把采集线程的 desktop handle 跨线程复用。
    thread_local DesktopAccess inputDesktop;
    thread_local auto nextDesktopCheck = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (!inputDesktop.IsBound()) {
        if (!inputDesktop.Bind()) {
            return;
        }
        nextDesktopCheck = now + std::chrono::seconds(1);
    } else if (now >= nextDesktopCheck) {
        nextDesktopCheck = now + std::chrono::seconds(1);
        inputDesktop.CheckRebind();
    }

    if (t == "key") {
        int scanCode = 0;
        bool down = false;
        bool extended = false;
        if (!ReadJsonIntInRange(j, "sc", 1, 0x7F, scanCode) ||
            !ReadJsonBool(j, "down", down) ||
            !ReadOptionalJsonBool(j, "ext", extended)) {
            return;
        }
        Input::SendKey(static_cast<USHORT>(scanCode), down, extended);
    } else if (t == "mouse") {
        double x = 0.0;
        double y = 0.0;
        std::string button;
        bool down = false;
        if (!ReadJsonFiniteNumber(j, "x", x) || !ReadJsonFiniteNumber(j, "y", y) ||
            !ReadJsonString(j, "btn", button) || !ReadJsonBool(j, "down", down)) {
            return;
        }
        double virtualX = 0.0;
        double virtualY = 0.0;
        if (!capture_.MapNormalizedToVirtual(x, y, virtualX, virtualY)) {
            return;
        }
        Input::SendMouseAbs(virtualX, virtualY);
        Input::SendMouseButton(button, down);
    } else if (t == "move") {
        double x = 0.0;
        double y = 0.0;
        if (!ReadJsonFiniteNumber(j, "x", x) || !ReadJsonFiniteNumber(j, "y", y)) {
            return;
        }
        double virtualX = 0.0;
        double virtualY = 0.0;
        if (capture_.MapNormalizedToVirtual(x, y, virtualX, virtualY)) {
            Input::SendMouseAbs(virtualX, virtualY);
        }
    } else if (t == "wheel") {
        int delta = 0;
        if (!ReadJsonIntInRange(j, "delta", -1200, 1200, delta)) {
            return;
        }
        Input::SendWheel(delta);
    }
}

void Agent::OnControllerDisconnected() {
    // 该回调在接收输入的 WebSocket 工作线程中运行。若本连接曾注入过输入，
    // 线程已经绑定了对应 desktop，可直接释放远端残留的按下状态。
    Input::ReleaseAll();
    // 即使控制端快速重连，以 CaptureLoop 未观察到“零客户端”为例，也要确保
    // 下一帧从 IDR 开始，并恢复下一位控制者的质量上限。
    frameResetRequested_.store(true);
    qualityResetRequested_.store(true);
}

}  // namespace remote_assist
