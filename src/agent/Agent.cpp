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
    uint64_t noChangeFrames = 0;
    uint64_t captureFailures = 0;
    uint64_t fingerprintSkipped = 0;
    uint64_t encodeAttempts = 0;
    uint64_t encodedFrames = 0;
    uint64_t encodeFailures = 0;
    uint64_t encodedBytes = 0;
    uint64_t captureTimeMs = 0;
    uint64_t encodeTimeMs = 0;
};

uint64_t CounterDelta(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : 0;
}

}  // namespace

Agent::~Agent() {
    Stop();
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
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
        // 退回到 share/remote-assist/web(便于 install 部署形态)
        std::wstring alt = dir + L"\\..\\share\\remote-assist\\web";
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

    instanceMutex_ = CreateMutexW(nullptr, FALSE, runtime::kAgentMutexName);
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
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);


    cfg_ = LoadOrCreateConfig();
    if (cfg_.passwordHash.empty() || cfg_.salt.empty()) {
        log::Error("agent: valid configuration unavailable");
        CoUninitialize();
        return 1;
    }
    log::Info("agent config port=" + std::to_string(cfg_.port) +
              " fps=" + std::to_string(cfg_.fps));

    // 显示器清单在启动网络服务前完成,后续仅由采集线程读取。
    capture_.EnumMonitors();

    server_.SetWebDir(WebDirFromExe());
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

    captureThread_ = std::thread(&Agent::CaptureLoop, this);

    // 主线程等待停止信号(MVP:轮询 stop_ 标志,后续可改事件)。
    while (!stop_.load()) {
        const DWORD waitResult = stopEvent_
            ? WaitForSingleObject(stopEvent_, 200)
            : WAIT_TIMEOUT;
        if (waitResult == WAIT_OBJECT_0) {
            log::Info("agent stop event received");
            Stop();
        }
    }

    Stop();
    CoUninitialize();
    log::Info("agent exit");
    return 0;
}

void Agent::Stop() {
    if (stop_.exchange(true)) {
        return;
    }
    // 服务停止不应依赖浏览器正常发送 keyup。主线程临时绑定当前输入桌面，
    // 尽早释放控制端遗留的修饰键和鼠标按键，再等待 WebSocket 工作线程退出。
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
    CaptureLoopStats metrics;
    BroadcasterStats broadcasterStatsBase = server_.Broadcaster().SnapshotStats();

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
        const uint64_t ackTimeouts = CounterDelta(broadcasterStats.ackTimeouts,
                                                  broadcasterStatsBase.ackTimeouts);
        const uint64_t sendFailures = CounterDelta(broadcasterStats.sendFailures,
                                                   broadcasterStatsBase.sendFailures);
        const bool active = metrics.captureAttempts != 0 || queued != 0 || sent != 0 ||
            acknowledged != 0 || ackTimeouts != 0 || sendFailures != 0;
        if (active) {
            const auto elapsedMs = std::max<int64_t>(1,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - metricsStartedAt).count());
            const uint64_t avgCaptureMs = metrics.captureAttempts == 0 ? 0 :
                metrics.captureTimeMs / metrics.captureAttempts;
            const uint64_t avgEncodeMs = metrics.encodeAttempts == 0 ? 0 :
                metrics.encodeTimeMs / metrics.encodeAttempts;
            log::Info("stream metrics " + std::to_string(elapsedMs) + "ms clients=" +
                      std::to_string(server_.Broadcaster().Count()) +
                      " capture=" + std::to_string(metrics.capturedFrames) + "/" +
                      std::to_string(metrics.captureAttempts) +
                      " unchanged=" + std::to_string(metrics.noChangeFrames) +
                      " sampled_skip=" + std::to_string(metrics.fingerprintSkipped) +
                      " capture_fail=" + std::to_string(metrics.captureFailures) +
                      " capture_avg_ms=" + std::to_string(avgCaptureMs) +
                      " encode=" + std::to_string(metrics.encodedFrames) + "/" +
                      std::to_string(metrics.encodeAttempts) +
                      " encode_fail=" + std::to_string(metrics.encodeFailures) +
                      " encode_avg_ms=" + std::to_string(avgEncodeMs) +
                      " jpeg_kib=" + std::to_string(metrics.encodedBytes / 1024) +
                      " queued=" + std::to_string(queued) +
                      " replaced=" + std::to_string(replaced) +
                      " sent=" + std::to_string(sent) +
                      " sent_kib=" + std::to_string(sentBytes / 1024) +
                      " ack=" + std::to_string(acknowledged) +
                      " ack_timeout=" + std::to_string(ackTimeouts) +
                      " send_fail=" + std::to_string(sendFailures));
        }
        metrics = {};
        broadcasterStatsBase = broadcasterStats;
        metricsStartedAt = now;
        nextMetricsLog = now + std::chrono::seconds(10);
    };

    while (!stop_.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        logMetricsIfDue(t0);

        // 大屏幕降低帧率,平衡编码时间和带宽
        int effectiveFps = cfg_.fps;
        if (deskWidth_.load() >= 1920 || deskHeight_.load() >= 1080) {
            effectiveFps = std::min(effectiveFps, 15);
        }
        const int targetMs = 1000 / std::max(1, effectiveFps);

        // 跟随桌面切换(锁屏↔解锁、Winlogon↔Default)。
        // 桌面检查是较重的系统调用,每秒一次即可覆盖切换场景。
        if (t0 >= nextDesktopCheck) {
            nextDesktopCheck = t0 + std::chrono::seconds(1);
            if (captureDesktop.CheckRebind()) {
                capture_.ResetForDesktop();
                firstFrame_ = true;
                lastFrameSent_ = {};
            }
        }

        // 没有控制端时不做抓屏、缩放和 JPEG 编码，既避免空转占用 CPU，
        // 也在下一次客户端连接时强制重新输出首帧。
        if (server_.Broadcaster().Count() == 0) {
            if (!firstFrame_) {
                firstFrame_ = true;
            }
            lastFrameSent_ = {};
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (frameResetRequested_.exchange(false)) {
            firstFrame_ = true;
            lastFrameSent_ = {};
        }

        CapturedFrame frame;
        // DXGI 空闲时会阻塞等待桌面变化。等待一个目标帧周期即可在静态画面
        // 下避免轮询空转，同时不会再因固定 50ms 把 30FPS 限制为约 20FPS。
        const auto captureStartedAt = std::chrono::steady_clock::now();
        const CaptureResult captureResult = capture_.CaptureFrame(
            frame, static_cast<DWORD>(std::max(1, targetMs)));
        metrics.captureAttempts++;
        metrics.captureTimeMs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - captureStartedAt).count());
        if (captureResult == CaptureResult::kNoChange) {
            metrics.noChangeFrames++;
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

        const uint64_t frameFingerprint = FrameFingerprint(frame.data);
        const bool refreshDue = lastFrameSent_.time_since_epoch().count() == 0 ||
            t0 - lastFrameSent_ >= std::chrono::seconds(1);
        if (!firstFrame_ && frameFingerprint == previousFrameFingerprint_ && !refreshDue) {
            metrics.fingerprintSkipped++;
            std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
            continue;
        }
        previousFrameFingerprint_ = frameFingerprint;
        firstFrame_ = false;

        if (!encoder_ || frame.width != deskWidth_.load() || frame.height != deskHeight_.load()) {
            deskWidth_.store(frame.width);
            deskHeight_.store(frame.height);
            encoderReady_ = false;
            encoder_ = std::make_unique<EncoderMf>();
            if (!encoder_->Init(frame.width, frame.height, cfg_.fps, cfg_.bitrate)) {
                log::Error("encoder init failed for " + std::to_string(frame.width) +
                           "x" + std::to_string(frame.height));
                encoder_.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
                continue;
            }
            encoderReady_ = true;
            // 尺寸/编码器变化后提升流版本并重新下发 cfg。浏览器异步图片解码可能
            // 在 cfg 到达后才完成，因此二进制帧会带上该版本供控制端判定。
            streamId_.fetch_add(1);
            server_.Broadcaster().BroadcastText(MakeCfgJson());
        }

        if (encoderReady_ && !frame.data.empty()) {
            std::vector<EncodedChunk> chunks;
            const auto encodeStartedAt = std::chrono::steady_clock::now();
            const bool encoded = encoder_->Encode(frame.data.data(), chunks);
            metrics.encodeAttempts++;
            metrics.encodeTimeMs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - encodeStartedAt).count());
            if (encoded) {
                bool sent = false;
                for (auto& c : chunks) {
                    if (c.data.empty()) {
                        continue;
                    }
                    metrics.encodedFrames++;
                    metrics.encodedBytes += c.data.size();
                    server_.Broadcaster().BroadcastBinary(std::move(c.data), streamId_.load());
                    sent = true;
                }
                if (sent) {
                    lastFrameSent_ = std::chrono::steady_clock::now();
                }
            } else {
                metrics.encodeFailures++;
            }
        }

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
    // 当前编码器固定输出 JPEG,避免跨线程读取 encoder_ 实例。
    j["codec"] = "jpeg";
    j["w"] = deskWidth_.load();
    j["h"] = deskHeight_.load();
    j["fps"] = cfg_.fps;
    j["stream_id"] = streamId_.load();
    auto mons = nlohmann::json::array();
    for (const auto& m : capture_.Monitors()) {
        mons.push_back({{"index", m.index}, {"name", m.name}, {"w", m.w}, {"h", m.h}});
    }
    j["monitors"] = mons;
    j["selected_monitor"] = capture_.SelectedMonitor();
    return j.dump();
}

void Agent::OnMessage(const std::string& msg) {
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

    auto j = nlohmann::json::parse(msg, nullptr, false);
    if (!j.is_object()) {
        return;
    }
    const std::string t = j.value("t", std::string());
    if (t == "key") {
        const int scanCode = j.value("sc", 0);
        if (scanCode <= 0 || scanCode > 0x7F) {
            return;
        }
        const USHORT sc = static_cast<USHORT>(scanCode);
        const bool down = j.value("down", false);
        const bool extended = j.value("ext", false);
        Input::SendKey(sc, down, extended);
    } else if (t == "mouse") {
        const double x = j.value("x", 0.0);
        const double y = j.value("y", 0.0);
        const std::string btn = j.value("btn", std::string());
        const bool down = j.value("down", false);
        double virtualX = 0.0;
        double virtualY = 0.0;
        if (!capture_.MapNormalizedToVirtual(x, y, virtualX, virtualY)) {
            return;
        }
        Input::SendMouseAbs(virtualX, virtualY);
        Input::SendMouseButton(btn, down);
    } else if (t == "move") {
        const double x = j.value("x", 0.0);
        const double y = j.value("y", 0.0);
        double virtualX = 0.0;
        double virtualY = 0.0;
        if (capture_.MapNormalizedToVirtual(x, y, virtualX, virtualY)) {
            Input::SendMouseAbs(virtualX, virtualY);
        }
    } else if (t == "wheel") {
        const int delta = std::clamp(j.value("delta", 0), -1200, 1200);
        Input::SendWheel(delta);
    } else if (t == "monitor") {
        const int idx = j.value("index", -1);
        capture_.SetMonitor(idx);
        frameResetRequested_.store(true);
        log::Info("monitor switched to idx=" + std::to_string(capture_.SelectedMonitor()));
    }
}

void Agent::OnControllerDisconnected() {
    // 该回调在接收输入的 WebSocket 工作线程中运行。若本连接曾注入过输入，
    // 线程已经绑定了对应 desktop，可直接释放远端残留的按下状态。
    Input::ReleaseAll();
}

}  // namespace remote_assist
