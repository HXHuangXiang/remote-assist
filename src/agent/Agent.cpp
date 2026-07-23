#include "agent/Agent.h"

#include "agent/DesktopAccess.h"
#include "agent/Input.h"
#include "common/Log.h"
#include "common/Path.h"
#include "common/RuntimeNames.h"
#include "common/StreamQuality.h"

#include <nlohmann/json.hpp>

#include <objbase.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#pragma comment(lib, "ole32.lib")

namespace remote_assist {

namespace {

// 采集线程私有统计，按窗口输出增量而非每帧打日志，既便于定位性能瓶颈又不会
// 因日志 I/O 干扰远控体验。
struct CaptureLoopStats {
    uint64_t captureAttempts = 0;
    uint64_t capturedFrames = 0;
    uint64_t directMappedFrames = 0;
    uint64_t gpuNv12Frames = 0;
    uint64_t gpuNv12SubmissionUs = 0;
    uint64_t noChangeFrames = 0;
    uint64_t pointerOnlyFrames = 0;
    uint64_t captureFailures = 0;
    uint64_t encodeAttempts = 0;
    uint64_t encodedFrames = 0;
    uint64_t encodeFailures = 0;
    uint64_t encodedBytes = 0;
    uint64_t captureTimeMs = 0;
    // captureTimeMs 包含 DXGI 等待桌面变化的正常阻塞；单独统计真正产出画面的
    // 采集耗时，才能区分“静态桌面在等帧”和“BitBlt/DXGI 本身过慢”。
    uint64_t capturedFrameTimeMs = 0;
    // CPU 回退分段指标：DXGI readback 后的复制/缩放与 GDI 系统调用。
    // 这些值只在相应路径真正发生时累加，避免 GPU 直通的零值稀释平均数。
    uint64_t cpuCopyOrScaleUs = 0;
    uint64_t cpuCopyOrScaleSamples = 0;
    uint64_t gdiBltUs = 0;
    uint64_t gdiBltSamples = 0;
    // CPU H.264 输入细分：颜色空间转换和每帧 MF buffer/sample 创建。MFT 实际
    // ProcessInput/ProcessOutput 耗时仍由 encodeTimeMs 统一统计。
    uint64_t bgraToNv12Us = 0;
    uint64_t bgraToNv12Samples = 0;
    uint64_t mfInputPreparationUs = 0;
    uint64_t mfInputPreparationSamples = 0;
    uint64_t d3dInputWrapUs = 0;
    uint64_t d3dInputWrapSamples = 0;
    uint64_t encodeTimeMs = 0;
    uint64_t h264CreditWaits = 0;
};

// GDI 屏幕复制即使画面不变也会同步读取选中的源区域。锁屏或跨显卡多屏长期
// 静止时没有必要持续占用高帧率；一旦收到有效的键鼠操作，先以保守 15 FPS
// 启动，再仅在实际 BitBlt 耗时足够低时逐级提高，以改善锁屏密码输入、点击反馈。
constexpr int kGdiInteractiveFpsInitialCap = 15;
constexpr int kGdiInteractiveFpsMaximumCap = 30;
constexpr int kGdiInteractiveFpsMinimumCap = 5;
constexpr int kGdiInteractiveFpsStep = 5;
constexpr int kGdiIdleFpsCap = 2;
constexpr uint64_t kGdiInputBoostWindowMs = 1200;
// 浏览器 mousemove 通常与显示器刷新同步（60Hz 或更高）。GDI 路径只需按初始
// 交互采样率唤醒即可既保证 hover 首帧及时，又避免完整 BitBlt 被事件频率拖高。
constexpr uint64_t kGdiMoveWakeIntervalMs =
    1000 / static_cast<uint64_t>(kGdiInteractiveFpsInitialCap);
// 桌面切换、显卡驱动重置或锁屏桌面暂时不可读时，采集若仍按 30 FPS 反复重建
// DXGI/GDI 资源，会把黑屏问题放大为高 CPU 占用和大量日志。首次失败尽快恢复，
// 随后以有界指数退避给桌面与驱动留下恢复时间。
constexpr int kCaptureFailureInitialBackoffMs = 250;
constexpr int kCaptureFailureMaxBackoffMs = 5000;
constexpr uint32_t kCaptureFailureReinitializeInterval = 4;
// 启动、重连和 ACK 超时恢复都要求先拿到真正的 IDR。少数硬件 MFT 会接受
// ForceKeyFrame/CleanPoint 请求却持续输出 P 帧；不能无限丢弃这些帧导致黑屏。
constexpr auto kH264KeyFrameWatchdogInterval = std::chrono::seconds(2);
constexpr int kH264KeyFrameMftRebuildLimit = 1;
// 某些硬件 MFT 只接受初始化时的平均码率。此时只能重建 MFT 才能让新的目标码率
// 生效；冷却窗口避免弱网下每秒销毁/重建编码器，造成连续 IDR、黑屏或驱动抖动。
constexpr auto kH264BitrateRebuildInterval = std::chrono::seconds(3);

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

// 浏览器静态资源可能在缓存中逐步升级。可选诊断字段缺失时保留调用方默认值，
// 但一旦提供仍按同样的范围与类型校验，避免旧页面兼容性成为无界输入通道。
bool ReadOptionalJsonIntInRange(const nlohmann::json& object, const char* key, int min,
                                int max, int& value) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
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

bool ParseStreamQuality(const std::string& text, StreamQuality& quality) {
    if (text == "auto") quality = StreamQuality::kAutomatic;
    else if (text == "original") quality = StreamQuality::kOriginal;
    else if (text == "1080p") quality = StreamQuality::k1080p;
    else if (text == "720p") quality = StreamQuality::k720p;
    else if (text == "540p") quality = StreamQuality::k540p;
    else if (text == "360p") quality = StreamQuality::k360p;
    else return false;
    return true;
}

const char* CursorStyleName(PointerCursorStyle style) {
    switch (style) {
    case PointerCursorStyle::kDefault: return "default";
    case PointerCursorStyle::kText: return "text";
    case PointerCursorStyle::kWait: return "wait";
    case PointerCursorStyle::kCrosshair: return "crosshair";
    case PointerCursorStyle::kPointer: return "pointer";
    case PointerCursorStyle::kMove: return "move";
    case PointerCursorStyle::kEastWestResize: return "ew-resize";
    case PointerCursorStyle::kNorthSouthResize: return "ns-resize";
    case PointerCursorStyle::kNorthwestSoutheastResize: return "nwse-resize";
    case PointerCursorStyle::kNortheastSouthwestResize: return "nesw-resize";
    case PointerCursorStyle::kNotAllowed: return "not-allowed";
    case PointerCursorStyle::kProgress: return "progress";
    case PointerCursorStyle::kHelp: return "help";
    }
    return "default";
}

// 将脏矩形扩展到固定网格，避免为几个像素分别创建高开销的 JPEG；网格面积才是
// 实际传输面积，因此以它和网页阈值比较，而非理想的原始脏矩形面积。
bool BuildJpegPatchTiles(const CapturedFrame& frame, int thresholdPercent,
                         EncoderMf& encoder, std::vector<JpegTile>& tiles) {
    tiles.clear();
    if (!frame.hasDirtyRegions || frame.dirtyRegions.empty() || frame.IsGpuNv12() ||
        !frame.Pixels() || frame.width <= 0 || frame.height <= 0 ||
        !IsPatchThresholdValid(thresholdPercent)) {
        return false;
    }
    constexpr int kTileSize = 64;
    const int columns = (frame.width + kTileSize - 1) / kTileSize;
    const int rows = (frame.height + kTileSize - 1) / kTileSize;
    std::vector<uint8_t> changed(static_cast<size_t>(columns) * rows, 0);
    for (const DirtyRegion& region : frame.dirtyRegions) {
        const int left = std::clamp(region.x, 0, frame.width);
        const int top = std::clamp(region.y, 0, frame.height);
        const int right = std::clamp(region.x + region.width, 0, frame.width);
        const int bottom = std::clamp(region.y + region.height, 0, frame.height);
        if (right <= left || bottom <= top) continue;
        const int firstColumn = left / kTileSize;
        const int lastColumn = (right - 1) / kTileSize;
        const int firstRow = top / kTileSize;
        const int lastRow = (bottom - 1) / kTileSize;
        for (int row = firstRow; row <= lastRow; ++row) {
            for (int column = firstColumn; column <= lastColumn; ++column) {
                changed[static_cast<size_t>(row) * columns + column] = 1;
            }
        }
    }
    uint64_t changedArea = 0;
    for (int row = 0; row < rows; ++row) {
        const int height = std::min(kTileSize, frame.height - row * kTileSize);
        for (int column = 0; column < columns; ++column) {
            if (changed[static_cast<size_t>(row) * columns + column] == 0) continue;
            const int width = std::min(kTileSize, frame.width - column * kTileSize);
            changedArea += static_cast<uint64_t>(width) * height;
        }
    }
    const uint64_t frameArea = static_cast<uint64_t>(frame.width) * frame.height;
    if (changedArea == 0 || changedArea * 100 >= frameArea * thresholdPercent) {
        return false;
    }
    // 同一行连续网格合成一个跨度；再将相邻行中跨度相同的区域纵向延伸。这样
    // 滚动、窗口拖动等连续变化不会生成数百个 64x64 JPEG，但所有边界仍对齐到
    // 固定网格，覆盖面积也仍是上面用于阈值比较的精确网格并集。
    struct TileBounds {
        int firstColumn = 0;
        int lastColumn = 0;
        int firstRow = 0;
        int lastRow = 0;
    };
    std::vector<TileBounds> rectangles;
    std::vector<TileBounds> active;
    for (int row = 0; row < rows; ++row) {
        std::vector<TileBounds> current;
        for (int column = 0; column < columns;) {
            if (changed[static_cast<size_t>(row) * columns + column] == 0) {
                ++column;
                continue;
            }
            const int firstColumn = column;
            while (column < columns &&
                   changed[static_cast<size_t>(row) * columns + column] != 0) {
                ++column;
            }
            const int lastColumn = column;
            const auto previous = std::find_if(active.begin(), active.end(),
                [firstColumn, lastColumn](const TileBounds& bounds) {
                    return bounds.firstColumn == firstColumn &&
                        bounds.lastColumn == lastColumn;
                });
            if (previous != active.end()) {
                TileBounds extended = *previous;
                extended.lastRow = row + 1;
                current.push_back(extended);
                active.erase(previous);
            } else {
                current.push_back({firstColumn, lastColumn, row, row + 1});
            }
        }
        rectangles.insert(rectangles.end(), active.begin(), active.end());
        active = std::move(current);
    }
    rectangles.insert(rectangles.end(), active.begin(), active.end());

    tiles.reserve(rectangles.size());
    for (const TileBounds& bounds : rectangles) {
        const int x = bounds.firstColumn * kTileSize;
        const int y = bounds.firstRow * kTileSize;
        const int width = std::min(frame.width, bounds.lastColumn * kTileSize) - x;
        const int height = std::min(frame.height, bounds.lastRow * kTileSize) - y;
        if (width <= 0 || height <= 0) {
            continue;
        }
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        for (int tileRow = 0; tileRow < height; ++tileRow) {
            std::memcpy(pixels.data() + static_cast<size_t>(tileRow) * width * 4,
                frame.Pixels() + static_cast<size_t>(y + tileRow) * frame.StrideBytes() +
                static_cast<size_t>(x) * 4, static_cast<size_t>(width) * 4);
        }
        JpegTile tile;
        tile.x = x;
        tile.y = y;
        tile.width = width;
        tile.height = height;
        if (!encoder.EncodeJpegTile(width, height, pixels.data(),
                                    static_cast<size_t>(width) * 4, tile.data)) {
            tiles.clear();
            return false;
        }
        tiles.push_back(std::move(tile));
    }
    return !tiles.empty();
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
    if (frameReadyEvent_) {
        CloseHandle(frameReadyEvent_);
        frameReadyEvent_ = nullptr;
    }
    if (gdiCaptureWakeEvent_) {
        CloseHandle(gdiCaptureWakeEvent_);
        gdiCaptureWakeEvent_ = nullptr;
    }
    if (instanceMutex_) {
        if (instanceMutexOwned_) {
            ReleaseMutex(instanceMutex_);
            instanceMutexOwned_ = false;
        }
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
}

std::string Agent::WebDirFromExe() {
    std::wstring dir = ModuleDirectory();
    if (dir.empty()) {
        return {};
    }
    dir += L"\\web";
    if (!PathFileExistsW(dir.c_str())) {
        // 兼容早期 CMake install 目录：exe 位于 bin/，网页在 prefix/share/。
        // 当前安装布局已统一为 exe 同级 web/，此分支仅用于平滑升级旧安装。
        std::wstring alt = dir + L"\\..\\..\\share\\remote-assist\\web";
        const std::wstring full = AbsolutePath(alt);
        if (!full.empty()) {
            if (PathFileExistsW(full.c_str())) {
                return Utf8FromWide(full);
            }
        }
    }
    return Utf8FromWide(dir);
}

int Agent::Run(bool serviceManaged) {
    log::Init(LogDir(), L"agent.log");
    log::Info("agent starting");

    // Service 先以受保护 ACL 创建全局 mutex，受管 Agent 只负责取得所有权。这样
    // 预创建同名对象不会让 Agent 继承未知 DACL；独立模式仍保留原有的自建互斥体，
    // 以便服务随后通过停止事件完成接管。
    if (serviceManaged) {
        instanceMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                    runtime::kAgentMutexName);
        if (!instanceMutex_) {
            log::Error("agent mutex open failed: " + std::to_string(GetLastError()));
            return 1;
        }
        const DWORD waitResult = WaitForSingleObject(instanceMutex_, 0);
        if (waitResult == WAIT_TIMEOUT) {
            log::Warn("agent already running, skip duplicate launch");
            return 0;
        }
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
            log::Error("agent mutex acquire failed: " + std::to_string(GetLastError()));
            return 1;
        }
        instanceMutexOwned_ = true;
    } else {
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
        instanceMutexOwned_ = true;
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
        frameReadyEvent_ = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                      runtime::kAgentFrameReadyEventName);
        if (!frameReadyEvent_) {
            log::Warn("agent frame-ready event open failed: " +
                      std::to_string(GetLastError()));
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

    const HRESULT mainComInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mainComInitialized = SUCCEEDED(mainComInit);
    if (FAILED(mainComInit) && mainComInit != RPC_E_CHANGED_MODE) {
        log::Warn("agent: CoInitializeEx failed hr=" + std::to_string(mainComInit));
    }
    const auto uninitializeMainCom = [&] {
        if (mainComInitialized) {
            CoUninitialize();
        }
    };

    cfg_ = LoadOrCreateConfig();
    if (cfg_.passwordHash.empty() || cfg_.salt.empty()) {
        log::Error("agent: valid configuration unavailable");
        uninitializeMainCom();
        return 1;
    }
    Input::Enable();
    // 自动复位的进程内事件只协调 Agent 的输入回调与采集线程，不暴露给外部
    // 进程。静态 GDI 画面可以低频采样，而每个有效远端输入都能立即唤醒它。
    gdiCaptureWakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gdiCaptureWakeEvent_) {
        log::Warn("agent: GDI capture wake event unavailable: " +
                  std::to_string(GetLastError()));
    }
    log::Info("agent config port=" + std::to_string(cfg_.port) +
              " fps=" + std::to_string(cfg_.fps));
    streamFps_.store(cfg_.fps);

    // 显示器清单在启动网络服务前完成,后续仅由采集线程读取。
    capture_.EnumMonitors();

    const std::string webDir = WebDirFromExe();
    if (!server_.SetWebDir(webDir)) {
        log::Error("agent: web resource directory unavailable: " + webDir);
        uninitializeMainCom();
        return 1;
    }
    server_.SetAuthVerifier([this](const std::string& token) {
        std::lock_guard<std::mutex> lock(passwordConfigMu_);
        return VerifyAndUpgradePassword(cfg_, token);
    });
    server_.SetCfgProvider([this]() { return MakeCfgJson(); });
    server_.SetOnMessage([this](const std::string& msg) { OnMessage(msg); });
    server_.SetOnControllerDisconnected([this]() { OnControllerDisconnected(); });

    if (!server_.Start("0.0.0.0", cfg_.port)) {
        log::Error("agent: server start failed");
        uninitializeMainCom();
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
    uninitializeMainCom();
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
    if (frameReadyEvent_) {
        ResetEvent(frameReadyEvent_);
    }
    if (gdiCaptureWakeEvent_) {
        SetEvent(gdiCaptureWakeEvent_);
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
    // Stop 可能在采集线程完成当前一帧编码时到达。join 后再次复位，避免服务停止后
    // 配置窗口仍读到上一轮“画面可用”的遗留状态。
    if (frameReadyEvent_) {
        ResetEvent(frameReadyEvent_);
    }
    // EncoderMf 在采集线程中调用 MFStartup；该线程退出前已经完成 Release/MFShutdown。
    // 此处 join 仅同步其生命周期，不能跨线程再次触碰 MFT。
}

void Agent::WakeGdiCaptureLoop(bool requestFreshFrame) {
    if (requestFreshFrame) {
        gdiFreshFrameRequested_.store(true, std::memory_order_release);
    }
    if (gdiCaptureWakeEvent_) {
        SetEvent(gdiCaptureWakeEvent_);
    }
}

void Agent::WaitForGdiCaptureDelay(int delayMs) const {
    const DWORD waitMs = static_cast<DWORD>(std::max(1, delayMs));
    if (gdiCaptureWakeEvent_) {
        // 关闭 Agent 时 Stop 也会置位该事件，因此不必额外轮询 stop_；下一轮
        // while 条件会直接结束采集线程。
        WaitForSingleObject(gdiCaptureWakeEvent_, waitMs);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
}

void Agent::CaptureLoop() {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comInit);
    if (FAILED(comInit) && comInit != RPC_E_CHANGED_MODE) {
        log::Warn("capture thread: CoInitializeEx failed hr=" + std::to_string(comInit));
    }
    DesktopAccess captureDesktop;
    bool captureDesktopAvailable = captureDesktop.Bind();
    if (!captureDesktopAvailable) {
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
    int pendingEncoderBitrateRebuild = 0;
    auto nextEncoderBitrateRebuildAt = nextDesktopCheck;
    // 自动模式从最高档起步并可按压力下调；固定档位锁定分辨率，原始清晰度则
    // 取消输出上限。网页偏好只保留在当前控制端，不写入 cfg_。
    StreamQuality activeQuality = StreamQuality::kAutomatic;
    int maximumQualityResolutionTier = ResolutionTierForStreamQuality(activeQuality);
    int adaptiveResolutionTier = maximumQualityResolutionTier;
    bool resolutionLocked = false;
    bool patchCapability = false;
    int patchThreshold = kDefaultPatchThresholdPercent;
    uint64_t appliedPreferenceGeneration = 0;
    capture_.SetMaxOutputSize(kStreamResolutionCaps[adaptiveResolutionTier].width,
                              kStreamResolutionCaps[adaptiveResolutionTier].height);
    int congestionWindows = 0;
    int healthyResolutionWindows = 0;
    const int maximumGdiInteractiveFpsCap = std::max(kGdiInteractiveFpsMinimumCap,
        std::min(kGdiInteractiveFpsMaximumCap, cfg_.fps));
    int gdiInteractiveFpsCap = std::min(kGdiInteractiveFpsInitialCap,
                                        maximumGdiInteractiveFpsCap);
    // metrics 会每十秒清零，不能直接拿它作为一秒自适应窗口。编码耗时只累计
    // 真正调用编码器的时间，不包含 DXGI 等待桌面变化的阻塞时间。
    uint64_t adaptiveEncodeAttempts = 0;
    uint64_t adaptiveEncodeTimeMs = 0;
    uint64_t adaptiveH264CreditWaits = 0;
    // DXGI 的 CaptureFrame 耗时包含“等待桌面变化”的正常阻塞，不能直接用它判断
    // 采集过载；GDI 每次调用都会同步执行 BitBlt/StretchBlt，即使指纹判断画面
    // 未变化也已经产生完整复制成本，因此必须按所有成功采样而非仅编码帧计量。
    uint64_t adaptiveGdiCaptureSamples = 0;
    uint64_t adaptiveGdiCaptureTimeMs = 0;
    uint64_t lastEncodeLoadPercent = 0;
    uint64_t lastGdiCaptureLoadPercent = 0;
    uint64_t encoderDeviceGeneration = 0;
    uint32_t consecutiveCaptureFailures = 0;
    std::chrono::steady_clock::time_point h264KeyFrameWaitingSince{};
    int h264KeyFrameRecoveryAttempts = 0;
    // 采集结果在相邻帧间复用容量。1920x1080 的 BGRA 帧约 8MB，若每轮都创建
    // 局部 CapturedFrame 会触发持续的大块堆分配和释放，直接放大卡顿。
    CapturedFrame frame;
    // 端口监听成功并不代表桌面绑定、采集与编码都可用；只有第一个可发送的编码
    // 帧入队后，才报告画面管线可用。
    bool framePipelineReady = false;
    // OpenInputDesktop 在锁屏切换的极短临界窗口可能失败。此时保留旧 HDESK 并不
    // 等于仍可安全采集它；恢复到可验证的当前 input desktop 前必须暂停画面管线。
    bool pendingDesktopCaptureReset = !captureDesktopAvailable;
    bool pendingMonitorConfigBroadcast = false;

    // frame-ready 代表“当前视频流”已经成功走到编码并入队。桌面、显示器、输出
    // 分辨率或编码器发生变化后，旧流的成功状态不能继续代表新流可用；集中复位
    // 可避免配置窗口在新流黑屏时仍显示上一轮的“画面管线已就绪”。
    auto resetFramePipelineReady = [&](const char* reason) {
        const bool wasReady = framePipelineReady;
        framePipelineReady = false;
        if (frameReadyEvent_) {
            ResetEvent(frameReadyEvent_);
        }
        if (wasReady) {
            log::Info(std::string("capture pipeline reset: ") + reason);
        }
    };

    auto effectiveFpsFor = [&](int requestedFps) {
        int effectiveFps = requestedFps;
        if (capture_.IsUsingGdi()) {
            const uint64_t nowTick = static_cast<uint64_t>(GetTickCount64());
            const uint64_t lastInputTick =
                lastRemoteInputTick_.load(std::memory_order_relaxed);
            const bool interactive = firstFrame_ ||
                (lastInputTick != 0 && nowTick - lastInputTick <= kGdiInputBoostWindowMs);
            // BitBlt 即使画面未变也需要整帧复制到 DIB。首帧与远端操作后的短窗口
            // 使用已经验证过的动态上限；锁屏静止画面仍降到 2 FPS，光标通过独立
            // 小消息及时更新。
            effectiveFps = std::min(effectiveFps,
                interactive ? gdiInteractiveFpsCap : kGdiIdleFpsCap);
        } else if ((!encoder_ || !encoder_->IsH264()) &&
                   (deskWidth_.load() >= 1920 || deskHeight_.load() >= 1080)) {
            effectiveFps = std::min(effectiveFps, 15);
        }
        return std::max(1, effectiveFps);
    };

    auto setResolutionTier = [&](int tier) {
        if (resolutionLocked) {
            return;
        }
        tier = std::clamp(tier, maximumQualityResolutionTier,
                          static_cast<int>(kStreamResolutionCaps.size()) - 1);
        if (tier == adaptiveResolutionTier) {
            return;
        }
        const StreamResolutionCap previous = kStreamResolutionCaps[adaptiveResolutionTier];
        adaptiveResolutionTier = tier;
        const StreamResolutionCap current = kStreamResolutionCaps[adaptiveResolutionTier];
        resetFramePipelineReady("adaptive resolution changed");
        // 输出上限改变后，静态 DXGI 桌面不会再自然产生一张帧。请求完整基线可让
        // 编码器立刻按新尺寸重建并下发 cfg，而不是等到用户下一次操作才生效。
        firstFrame_ = true;
        streamKeyFrameRequired_ = true;
        capture_.SetMaxOutputSize(current.width, current.height);
        log::Info("adaptive stream cap " + std::to_string(previous.width) + "x" +
                  std::to_string(previous.height) + " -> " +
                  std::to_string(current.width) + "x" + std::to_string(current.height));
    };

    auto applyStreamPreferences = [&] {
        const uint64_t generation = streamPreferenceGeneration_.load(std::memory_order_acquire);
        if (generation == appliedPreferenceGeneration) {
            return;
        }
        appliedPreferenceGeneration = generation;
        int qualityValue = requestedStreamQuality_.load(std::memory_order_relaxed);
        if (!IsStreamQualityValid(qualityValue)) {
            qualityValue = static_cast<int>(StreamQuality::kAutomatic);
        }
        const StreamQuality nextQuality = static_cast<StreamQuality>(qualityValue);
        int nextThreshold = requestedPatchThreshold_.load(std::memory_order_relaxed);
        if (!IsPatchThresholdValid(nextThreshold)) {
            nextThreshold = kDefaultPatchThresholdPercent;
        }
        const bool nextPatchCapability =
            requestedPatchCapability_.load(std::memory_order_relaxed);
        const bool qualityChanged = nextQuality != activeQuality;
        const bool patchCapabilityChanged = nextPatchCapability != patchCapability;
        activeQuality = nextQuality;
        patchThreshold = nextThreshold;
        patchCapability = nextPatchCapability;
        capture_.SetPatchCaptureEnabled(patchCapability);
        if (!qualityChanged && !patchCapabilityChanged) {
            return;
        }

        maximumQualityResolutionTier = ResolutionTierForStreamQuality(activeQuality);
        adaptiveResolutionTier = maximumQualityResolutionTier;
        resolutionLocked = IsFixedStreamQuality(activeQuality);
        if (activeQuality == StreamQuality::kOriginal) {
            capture_.SetMaxOutputSize(std::numeric_limits<int>::max(),
                                      std::numeric_limits<int>::max());
        } else {
            const StreamResolutionCap cap = kStreamResolutionCaps[adaptiveResolutionTier];
            capture_.SetMaxOutputSize(cap.width, cap.height);
        }
        firstFrame_ = true;
        streamKeyFrameRequired_ = true;
        resetFramePipelineReady("web stream preference changed");
        // 即便新旧输出分辨率恰好相同，也要让浏览器丢弃旧异步解码/图块任务；随后
        // 的首帧会请求独立 IDR，保证网页画布拥有可供局部更新的完整基线。
        streamId_.fetch_add(1);
        server_.Broadcaster().BroadcastText(MakeCfgJson());
        log::Info(std::string("web stream preference quality=") +
                  StreamQualityName(activeQuality) + " patch=" +
                  (patchCapability ? "true" : "false") + " threshold=" +
                  std::to_string(patchThreshold));
    };

    // 自适应码率是按“当前控制端”的链路质量得出的。控制端断开后若直接保留低码率，
    // 下一台网络正常的电脑会长时间看到低质量画面；空闲期重置不影响正在播放的流。
    auto restoreBitrateForNextController = [&] {
        // 上一位控制端留下的低码率重建请求不能带到新会话；空闲期恢复配置上限时，
        // 下面现有逻辑会选择运行期更新或安全地重新初始化编码器。
        pendingEncoderBitrateRebuild = 0;
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
        if (!resolutionLocked && adaptiveResolutionTier != maximumQualityResolutionTier) {
            setResolutionTier(maximumQualityResolutionTier);
        }
        congestionWindows = 0;
        healthyResolutionWindows = 0;
    };

    auto captureFailureBackoffMs = [](uint32_t failures, int targetMs) {
        // failures 从 1 开始，250/500/1000/2000/4000/5000ms 递增；目标帧间隔
        // 较大时仍保持其下限，避免低 FPS 配置下意外提升重试频率。
        const uint32_t shift = std::min<uint32_t>(failures > 0 ? failures - 1 : 0, 5);
        const int backoff = kCaptureFailureInitialBackoffMs << shift;
        return std::min(kCaptureFailureMaxBackoffMs, std::max(targetMs, backoff));
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
        const uint64_t clientDecodeMsTotal =
            clientDecodeMsTotal_.exchange(0, std::memory_order_relaxed);
        const uint64_t clientDecodeMsSamples =
            clientDecodeMsSamples_.exchange(0, std::memory_order_relaxed);
        const uint64_t clientDecodeErrors =
            clientDecodeErrors_.exchange(0, std::memory_order_relaxed);
        const uint32_t clientMaxDecodeQueue =
            clientMaxDecodeQueue_.exchange(0, std::memory_order_relaxed);
        const uint32_t clientMaxWsBuffered =
            clientMaxWsBufferedBytes_.exchange(0, std::memory_order_relaxed);
        const bool active = metrics.captureAttempts != 0 || queued != 0 || sent != 0 ||
            acknowledged != 0 || ackTimeouts != 0 || sendFailures != 0 || h264Resyncs != 0 ||
            metrics.h264CreditWaits != 0 || clientDrawn != 0 || clientDropped != 0 ||
            clientDecodeMsSamples != 0 || clientDecodeErrors != 0;
        if (active) {
            const auto elapsedMs = std::max<int64_t>(1,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - metricsStartedAt).count());
            const uint64_t avgCaptureMs = metrics.captureAttempts == 0 ? 0 :
                metrics.captureTimeMs / metrics.captureAttempts;
            const uint64_t avgCapturedFrameMs = metrics.capturedFrames == 0 ? 0 :
                metrics.capturedFrameTimeMs / metrics.capturedFrames;
            const uint64_t avgEncodeMs = metrics.encodeAttempts == 0 ? 0 :
                metrics.encodeTimeMs / metrics.encodeAttempts;
            const auto averageStageUs = [](uint64_t totalUs, uint64_t samples) {
                return samples == 0 ? uint64_t{0} : totalUs / samples;
            };
            const uint64_t avgCpuCopyOrScaleUs = averageStageUs(
                metrics.cpuCopyOrScaleUs, metrics.cpuCopyOrScaleSamples);
            const uint64_t avgGdiBltUs = averageStageUs(metrics.gdiBltUs,
                                                        metrics.gdiBltSamples);
            const uint64_t avgBgraToNv12Us = averageStageUs(metrics.bgraToNv12Us,
                                                            metrics.bgraToNv12Samples);
            const uint64_t avgMfInputPreparationUs = averageStageUs(
                metrics.mfInputPreparationUs, metrics.mfInputPreparationSamples);
            const uint64_t avgD3dInputWrapUs = averageStageUs(metrics.d3dInputWrapUs,
                                                              metrics.d3dInputWrapSamples);
            const uint64_t avgGpuNv12SubmissionUs = averageStageUs(
                metrics.gpuNv12SubmissionUs, metrics.gpuNv12Frames);
            const uint64_t avgAckMs = ackLatencySamples == 0 ? 0 :
                ackLatencyUs / ackLatencySamples / 1000;
            const uint64_t clientDrawAvgMs = clientDrawMsSamples == 0 ? 0 :
                clientDrawMsTotal / clientDrawMsSamples;
            const uint64_t clientDecodeAvgMs = clientDecodeMsSamples == 0 ? 0 :
                clientDecodeMsTotal / clientDecodeMsSamples;
            const int effectiveFps = effectiveFpsFor(adaptiveFps);
            const uint64_t targetFrameMs = 1000 / std::max(1, effectiveFps);
            const uint64_t captureLoadPercent =
                avgCapturedFrameMs * static_cast<uint64_t>(effectiveFps) * 100 / 1000;
            const uint64_t encodeWindowLoadPercent =
                avgEncodeMs * static_cast<uint64_t>(effectiveFps) * 100 / 1000;
            std::string diagnosis;
            const auto appendDiagnosis = [&diagnosis](const char* value) {
                if (!diagnosis.empty()) {
                    diagnosis += ',';
                }
                diagnosis += value;
            };
            // 诊断以端到端流水线的不同阶段为单位。保守阈值避免把 DXGI 的空闲
            // 等待、一次关键帧或浏览器正常 rAF 误报成性能瓶颈；多个条件同时成立
            // 时同时输出，方便从同一条日志看到级联问题。
            if (metrics.capturedFrames >= 2 && captureLoadPercent >= 80) {
                appendDiagnosis(capture_.IsUsingGdi() ? "capture_gdi" : "capture_dxgi");
            }
            if (metrics.encodeAttempts >= 2 && encodeWindowLoadPercent >= 85) {
                appendDiagnosis("encode");
            }
            if (clientDecodeErrors != 0 || clientMaxDecodeQueue > 2 ||
                (clientDecodeMsSamples >= 2 &&
                 clientDecodeAvgMs >= std::max<uint64_t>(50, targetFrameMs * 2)) ||
                (clientDrawMsSamples >= 2 &&
                 clientDrawAvgMs >= std::max<uint64_t>(80, targetFrameMs * 2))) {
                appendDiagnosis("browser");
            }
            if (ackTimeouts != 0 || sendFailures != 0 || h264Resyncs != 0 ||
                metrics.h264CreditWaits != 0 ||
                (ackLatencySamples >= 2 && avgAckMs >= 120)) {
                appendDiagnosis("network");
            }
            if (diagnosis.empty()) {
                diagnosis = "normal";
            }
            log::Info("stream metrics " + std::to_string(elapsedMs) + "ms clients=" +
                      std::to_string(server_.Broadcaster().Count()) +
                      " capture=" + std::to_string(metrics.capturedFrames) + "/" +
                      std::to_string(metrics.captureAttempts) +
                      " direct=" + std::to_string(metrics.directMappedFrames) +
                      " gpu_nv12=" + std::to_string(metrics.gpuNv12Frames) +
                      " gpu_nv12_submit_avg_us=" +
                      std::to_string(avgGpuNv12SubmissionUs) +
                      " unchanged=" + std::to_string(metrics.noChangeFrames) +
                      " pointer_only=" + std::to_string(metrics.pointerOnlyFrames) +
                      " capture_fail=" + std::to_string(metrics.captureFailures) +
                      " capture_avg_ms=" + std::to_string(avgCaptureMs) +
                      " capture_frame_avg_ms=" + std::to_string(avgCapturedFrameMs) +
                      " capture_cpu_copy_scale_avg_us=" +
                      std::to_string(avgCpuCopyOrScaleUs) +
                      " gdi_blt_avg_us=" + std::to_string(avgGdiBltUs) +
                      " capture_load_pct=" + std::to_string(captureLoadPercent) +
                      " encode=" + std::to_string(metrics.encodedFrames) + "/" +
                      std::to_string(metrics.encodeAttempts) +
                      " encode_fail=" + std::to_string(metrics.encodeFailures) +
                      " encode_avg_ms=" + std::to_string(avgEncodeMs) +
                      " bgra_nv12_avg_us=" + std::to_string(avgBgraToNv12Us) +
                      " mf_input_prepare_avg_us=" +
                      std::to_string(avgMfInputPreparationUs) +
                      " d3d_input_wrap_avg_us=" + std::to_string(avgD3dInputWrapUs) +
                      " encode_load_pct=" + std::to_string(lastEncodeLoadPercent) +
                      " encode_window_load_pct=" + std::to_string(encodeWindowLoadPercent) +
                      " adaptive_gdi_capture_load_pct=" +
                      std::to_string(lastGdiCaptureLoadPercent) +
                      " gdi_interactive_fps_cap=" +
                      std::to_string(gdiInteractiveFpsCap) +
                      " h264_credit_wait=" + std::to_string(metrics.h264CreditWaits) +
                      " encoded_kib=" + std::to_string(metrics.encodedBytes / 1024) +
                      " queued=" + std::to_string(queued) +
                      " replaced=" + std::to_string(replaced) +
                      " sent=" + std::to_string(sent) +
                      " sent_kib=" + std::to_string(sentBytes / 1024) +
                      " ack=" + std::to_string(acknowledged) +
                      " ack_avg_ms=" + std::to_string(avgAckMs) +
                      " ack_timeout=" + std::to_string(ackTimeouts) +
                      " h264_ack_window_ms=" +
                      std::to_string(broadcasterStats.h264AckTimeoutMs) +
                      " send_fail=" + std::to_string(sendFailures) +
                      " h264_resync=" + std::to_string(h264Resyncs) +
                      " client_drawn=" + std::to_string(clientDrawn) +
                      " client_dropped=" + std::to_string(clientDropped) +
                      " client_draw_avg_ms=" + std::to_string(clientDrawAvgMs) +
                      " client_decode_avg_ms=" + std::to_string(clientDecodeAvgMs) +
                      " client_decode_errors=" + std::to_string(clientDecodeErrors) +
                      " client_decode_queue_max=" + std::to_string(clientMaxDecodeQueue) +
                      " client_ws_buffered_max=" + std::to_string(clientMaxWsBuffered) +
                      " stream_fps=" + std::to_string(adaptiveFps) +
                      " stream_bitrate=" + std::to_string(adaptiveBitrate) +
                      " stream_cap=" +
                      std::to_string(kStreamResolutionCaps[adaptiveResolutionTier].width) + "x" +
                      std::to_string(kStreamResolutionCaps[adaptiveResolutionTier].height) +
                      " diagnosis=" + diagnosis);
        }
        metrics = {};
        broadcasterStatsBase = broadcasterStats;
        metricsStartedAt = now;
        nextMetricsLog = now + std::chrono::seconds(10);
    };

    // 发送队列只能安全保留很少的 H.264 增量帧；一旦浏览器绘制确认或本机编码
    // 追不上，继续以固定 FPS 编码只会反复触发 IDR 重同步。依据一秒窗口的网络
    // 和真实编码耗时快速降帧，网络/CPU 恢复后再平滑回升到用户配置上限。
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
        const uint64_t h264CreditWaits = adaptiveH264CreditWaits;
        const uint64_t averageAckMs = ackSamples == 0 ? 0 : ackLatencyUs / ackSamples / 1000;
        const int currentEffectiveFps = effectiveFpsFor(adaptiveFps);
        const uint64_t averageEncodeMs = adaptiveEncodeAttempts == 0 ? 0 :
            adaptiveEncodeTimeMs / adaptiveEncodeAttempts;
        const uint64_t averageGdiCaptureMs = adaptiveGdiCaptureSamples == 0 ? 0 :
            adaptiveGdiCaptureTimeMs / adaptiveGdiCaptureSamples;
        lastEncodeLoadPercent = averageEncodeMs * static_cast<uint64_t>(currentEffectiveFps) *
            100 / 1000;
        lastGdiCaptureLoadPercent = averageGdiCaptureMs *
            static_cast<uint64_t>(currentEffectiveFps) * 100 / 1000;
        // 保留约 15% 的编码时间预算给采集、内存传输与系统调度；只在至少有两帧
        // 样本时触发，避免切屏首帧或一次 IDR 的偶发耗时造成无谓降档。
        const bool encoderOverloaded = adaptiveEncodeAttempts >= 2 &&
            lastEncodeLoadPercent >= 85;
        const bool encoderHealthy = adaptiveEncodeAttempts == 0 ||
            (adaptiveEncodeAttempts >= 2 && lastEncodeLoadPercent <= 55);
        // 正常情况等两帧再判断；但一次 BitBlt 已经超过整个帧预算时，不能再等
        // 下一秒的第二个样本，否则超大分辨率/软件渲染锁屏会持续卡在原档位。
        const bool gdiCaptureOverloaded =
            (adaptiveGdiCaptureSamples >= 2 && lastGdiCaptureLoadPercent >= 85) ||
            (adaptiveGdiCaptureSamples >= 1 && lastGdiCaptureLoadPercent >= 110);
        const bool gdiCaptureHealthy = adaptiveGdiCaptureSamples == 0 ||
            (adaptiveGdiCaptureSamples >= 2 && lastGdiCaptureLoadPercent <= 55);
        const bool networkCongested = HasSustainedH264CreditBackpressure(h264CreditWaits) ||
            ackTimeouts != 0 || h264Resyncs != 0 || sendFailures != 0 ||
            (ackSamples != 0 && averageAckMs >= 120);
        // 80ms 内的稳定绘制确认仍足以让两帧 H.264 窗口在 30FPS 下连续前进。
        // 旧阈值 65ms 会让许多正常 LAN 浏览器只会降档、很难平滑恢复。
        const bool networkHealthy = ackSamples >= 4 && averageAckMs <= 80 && ackTimeouts == 0 &&
            h264Resyncs == 0 && sendFailures == 0;
        const bool congested = networkCongested || encoderOverloaded || gdiCaptureOverloaded;
        const bool healthy = networkHealthy && encoderHealthy && gdiCaptureHealthy;
        const bool severeCongestion = ackTimeouts != 0 || h264Resyncs != 0 || sendFailures != 0 ||
            lastEncodeLoadPercent >= 100 || lastGdiCaptureLoadPercent >= 110;

        // 锁屏/跨显卡的 GDI 路径此前固定为 15 FPS，即使一次屏幕复制只需几毫秒也
        // 无法改善交互观感。只根据实际 BitBlt 采样逐级提高，并在即将超过 55%
        // 时间预算时停止；任何采集过载都会立即降低上限。
        if (capture_.IsUsingGdi()) {
            const int previousCap = gdiInteractiveFpsCap;
            if (gdiCaptureOverloaded) {
                gdiInteractiveFpsCap = std::max(kGdiInteractiveFpsMinimumCap,
                    gdiInteractiveFpsCap * 3 / 4);
            } else if (adaptiveGdiCaptureSamples >= 2 && !networkCongested &&
                       encoderHealthy && averageGdiCaptureMs != 0) {
                const int candidateCap = std::min(maximumGdiInteractiveFpsCap,
                    gdiInteractiveFpsCap + kGdiInteractiveFpsStep);
                const uint64_t candidateLoadPercent = averageGdiCaptureMs *
                    static_cast<uint64_t>(candidateCap) * 100 / 1000;
                if (candidateLoadPercent <= 55) {
                    gdiInteractiveFpsCap = candidateCap;
                }
            }
            if (previousCap != gdiInteractiveFpsCap) {
                log::Info("adaptive GDI interactive fps cap " +
                          std::to_string(previousCap) + " -> " +
                          std::to_string(gdiInteractiveFpsCap) +
                          " capture_avg_ms=" + std::to_string(averageGdiCaptureMs) +
                          " capture_load_pct=" +
                          std::to_string(lastGdiCaptureLoadPercent));
            }
        }
        int nextFps = adaptiveFps;
        int nextBitrate = adaptiveBitrate;
        const int minimumBitrate = std::min(cfg_.bitrate,
            std::max(100'000, cfg_.bitrate / 8));
        if (congested) {
            // 乘法退避能在高延迟 Wi-Fi 下更快逃离“P 帧挤压 -> IDR”的循环。
            const int encoderLimitedFps = encoderOverloaded
                ? std::max(5, currentEffectiveFps * 3 / 4)
                : adaptiveFps;
            const int captureLimitedFps = gdiCaptureOverloaded
                ? std::max(5, currentEffectiveFps * 3 / 4)
                : adaptiveFps;
            nextFps = std::max(5, std::min({adaptiveFps * 3 / 4,
                                             encoderLimitedFps, captureLimitedFps}));
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
                adaptiveResolutionTier + 1 < static_cast<int>(kStreamResolutionCaps.size())) {
                setResolutionTier(adaptiveResolutionTier + 1);
                congestionWindows = 0;
            }
        } else if (healthy) {
            congestionWindows = 0;
            ++healthyResolutionWindows;
            // 恢复分辨率比恢复 FPS/码率更保守，连续健康五秒才上调一档。
            if (healthyResolutionWindows >= 5 &&
                adaptiveResolutionTier > maximumQualityResolutionTier) {
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
                      std::to_string(h264Resyncs) + " encode_avg_ms=" +
                      std::to_string(averageEncodeMs) + " encode_load_pct=" +
                      std::to_string(lastEncodeLoadPercent) + " encoder_overload=" +
                      (encoderOverloaded ? "true" : "false") + " h264_credit_wait=" +
                      std::to_string(h264CreditWaits) + " gdi_capture_avg_ms=" +
                      std::to_string(averageGdiCaptureMs) + " gdi_capture_load_pct=" +
                      std::to_string(lastGdiCaptureLoadPercent) + " capture_overload=" +
                      (gdiCaptureOverloaded ? "true" : "false"));
            adaptiveFps = nextFps;
            streamFps_.store(adaptiveFps);
        }
        if (nextBitrate != adaptiveBitrate && encoder_ && encoder_->IsH264()) {
            if (adaptiveBitrateAvailable) {
                if (encoder_->UpdateBitrate(nextBitrate)) {
                    adaptiveBitrate = nextBitrate;
                } else {
                    // 部分 MFT 只能在 SetOutputType 前写入码率。仅降 FPS 会让弱网
                    // 下的单帧体积长期停留在高档位，因此记录目标码率，稍后以受控
                    // 重建方式让它生效。
                    adaptiveBitrateAvailable = false;
                    // 质量恢复比降档更保守：连续三个健康窗口后才允许用重建提高
                    // 码率，避免一秒短暂恢复就反复触发 H.264 流重协商。
                    pendingEncoderBitrateRebuild =
                        (!healthy || healthyResolutionWindows >= 3) ? nextBitrate : 0;
                    log::Info("H.264 MFT does not support dynamic bitrate; scheduling encoder rebuild");
                }
            } else if ((congested && nextBitrate < adaptiveBitrate) ||
                       (healthy && healthyResolutionWindows >= 3 &&
                        nextBitrate > adaptiveBitrate)) {
                // 等待重建冷却期间继续用 FPS/分辨率保护实时性；只保留最新、更低的
                // 或更高的目标，不能为已过期的中间档位连续重建。
                pendingEncoderBitrateRebuild = nextBitrate;
            } else if (!congested) {
                pendingEncoderBitrateRebuild = 0;
            }
        }
        if (pendingEncoderBitrateRebuild > 0 &&
            now >= nextEncoderBitrateRebuildAt && encoder_ && encoder_->IsH264()) {
            const int targetBitrate = pendingEncoderBitrateRebuild;
            pendingEncoderBitrateRebuild = 0;
            if (targetBitrate != adaptiveBitrate) {
                const int previousBitrate = adaptiveBitrate;
                // 当前帧之前销毁旧 MFT；同一采集循环稍后会用 targetBitrate 创建新
                // 编码器、增加 stream_id、下发 cfg，并从独立 IDR 开始新 GOP。
                // 不能直接复用旧 H.264 参考帧，否则浏览器可能把不同码率/参数集的
                // delta 帧接到旧流上。
                resetFramePipelineReady("H.264 bitrate encoder rebuild");
                capture_.SetGpuOutputEnabled(false);
                encoder_->Release();
                encoder_.reset();
                encoderReady_ = false;
                streamKeyFrameRequired_ = true;
                encoderDeviceGeneration = 0;
                adaptiveBitrate = targetBitrate;
                adaptiveBitrateAvailable = true;
                nextEncoderBitrateRebuildAt = now + kH264BitrateRebuildInterval;
                log::Info("reinitializing H.264 encoder for bitrate " +
                          std::to_string(previousBitrate) + " -> " +
                          std::to_string(adaptiveBitrate));
            }
        }
        adaptationStatsBase = stats;
        adaptiveEncodeAttempts = 0;
        adaptiveEncodeTimeMs = 0;
        adaptiveH264CreditWaits = 0;
        adaptiveGdiCaptureSamples = 0;
        adaptiveGdiCaptureTimeMs = 0;
        nextAdaptation = now + std::chrono::seconds(1);
    };

    while (!stop_.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        logMetricsIfDue(t0);
        adaptFrameRateIfDue(t0);
        applyStreamPreferences();

        // JPEG 回退需要 CPU 全帧压缩，大屏时保守限帧；H.264 模式的压缩由 MFT
        // 承担，不应仅因分辨率达到 1080p 就被无条件锁在 15 FPS，否则既浪费
        // 硬件能力，也会让编码器的帧时间基和实际采集节奏不一致。
        const int effectiveFps = effectiveFpsFor(adaptiveFps);
        const int targetMs = 1000 / std::max(1, effectiveFps);

        // 跟随桌面切换(锁屏↔解锁、Winlogon↔Default)。
        // 桌面与显示器布局检查是较重的系统调用,每秒一次即可覆盖锁屏切换、热插拔
        // 和分辨率调整。布局变化即使输出尺寸未变，也必须重新下发 monitor 列表。
        if (t0 >= nextDesktopCheck) {
            nextDesktopCheck = t0 + std::chrono::seconds(1);
            const DesktopRebindResult rebindResult = captureDesktop.CheckRebind();
            const bool desktopWasAvailable = captureDesktopAvailable;
            if (rebindResult == DesktopRebindResult::kFailed) {
                captureDesktopAvailable = false;
                if (desktopWasAvailable) {
                    // 不继续消费旧 desktop 的 DXGI/GDI 帧。否则锁屏/解锁临界窗口
                    // 可能把已失效画面继续编码和发送，看起来像“卡住的旧桌面”。
                    pendingDesktopCaptureReset = true;
                    firstFrame_ = true;
                    streamKeyFrameRequired_ = true;
                    resetFramePipelineReady("input desktop unavailable");
                    log::Warn("capture paused: unable to verify current input desktop");
                }
            } else {
                captureDesktopAvailable = true;
                if (!desktopWasAvailable || rebindResult == DesktopRebindResult::kRebound) {
                    pendingDesktopCaptureReset = true;
                    log::Info("capture desktop verified; scheduling capture resource reset");
                }
            }
            const bool displayChanged = capture_.EnumMonitors();
            if (displayChanged) {
                pendingDesktopCaptureReset = true;
                pendingMonitorConfigBroadcast = true;
            }
            if (captureDesktopAvailable && pendingDesktopCaptureReset) {
                // DXGI desktop 重建会产生新的 D3D11 device。保留绑定旧 device 的
                // MFT 会让下一张 GPU surface 发生跨设备输入；提前释放，后续帧会
                // 按当前 device 重新协商，CPU/GDI 回退不受影响。
                if (encoder_ && encoder_->CanEncodeD3D11()) {
                    encoder_->Release();
                    encoder_.reset();
                    encoderReady_ = false;
                    encoderDeviceGeneration = 0;
                }
                capture_.ResetForDesktop();
                firstFrame_ = true;
                streamKeyFrameRequired_ = true;
                resetFramePipelineReady("desktop or display changed");
                pendingDesktopCaptureReset = false;
                if (pendingMonitorConfigBroadcast) {
                    server_.Broadcaster().BroadcastText(MakeCfgJson());
                    pendingMonitorConfigBroadcast = false;
                }
            }
        }

        // 断连回调若刚好无法绑定当前 desktop，会把释放请求保留到这里。不能只
        // 依赖上方的秒级轮询：锁屏/解锁可能恰好发生在检查后的数百毫秒内，必须
        // 在注入 keyup/mouseup 前再确认一次当前 input desktop。
        if (inputReleaseRequested_.load(std::memory_order_acquire)) {
            const DesktopRebindResult releaseRebindResult = captureDesktop.CheckRebind();
            if (releaseRebindResult == DesktopRebindResult::kFailed) {
                if (captureDesktopAvailable) {
                    captureDesktopAvailable = false;
                    pendingDesktopCaptureReset = true;
                    firstFrame_ = true;
                    streamKeyFrameRequired_ = true;
                    resetFramePipelineReady("input desktop unavailable during release");
                    log::Warn("input release paused: unable to verify current input desktop");
                }
            } else {
                captureDesktopAvailable = true;
                if (releaseRebindResult == DesktopRebindResult::kRebound) {
                    pendingDesktopCaptureReset = true;
                    firstFrame_ = true;
                    streamKeyFrameRequired_ = true;
                    // 下一轮立即重建桌面资源，不能继续采集已经切换前的 surface。
                    nextDesktopCheck = t0;
                }
                if (Input::ReleaseAll()) {
                    if (inputReleaseRequested_.exchange(false, std::memory_order_acq_rel)) {
                        server_.Broadcaster().CompleteControllerInputCleanup();
                        log::Info("released remote input after desktop binding recovered");
                    }
                } else {
                    // 保留请求与控制端清理态，后续在已验证的 desktop 上继续重试。
                    log::Warn("remote input release incomplete; retrying after desktop check");
                }
            }
        }

        if (!captureDesktopAvailable) {
            // 不允许在旧 desktop 上继续 CaptureFrame。短等待既限制临界窗口内的
            // 重试开销，也让 Stop() 通过 GDI 唤醒事件立即结束采集线程。
            WaitForGdiCaptureDelay(100);
            continue;
        }

        // 没有控制端时不做抓屏、缩放和 JPEG 编码，既避免空转占用 CPU，
        // 也在下一次客户端连接时强制重新输出首帧。
        if (server_.Broadcaster().Count() == 0) {
            if (!firstFrame_) {
                firstFrame_ = true;
            }
            // 编码器仍会保留上一控制端的参考帧；新控制端绝不能从 delta 帧接续。
            streamKeyFrameRequired_ = true;
            restoreQualityForNextController();
            // Add() 会在控制端完成鉴权和注册时立即唤醒，首帧不再受空闲轮询周期
            // 限制；无控制端时仍保持最多 200ms 的低 CPU 等待。
            server_.Broadcaster().WaitForActiveController(std::chrono::milliseconds(200));
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
            resetFramePipelineReady("controller requested stream reset");
        }
        const auto consumeH264ResyncRequest = [&] {
            if (!server_.Broadcaster().ConsumeH264ResyncRequest()) {
                return false;
            }
            // ACK 超时表示浏览器已经无法在实时窗口内呈现旧预测帧。发送线程只
            // 负责清理队列；MFT 只能由采集线程操作，因此在这里重建可独立解码的
            // 首帧，并跳过旧 GOP 的后续 delta。
            firstFrame_ = true;
            streamKeyFrameRequired_ = true;
            if (encoder_ && encoder_->IsH264()) {
                encoder_->RequestKeyFrame();
            }
            resetFramePipelineReady("H.264 frame acknowledgement timed out");
            log::Warn("H.264 ack timeout consumed, requesting fresh IDR");
            return true;
        };
        consumeH264ResyncRequest();
        const auto consumePatchResyncRequest = [&] {
            if (!server_.Broadcaster().ConsumePatchResyncRequest()) {
                return false;
            }
            // 局部批次可能已部分绘制，不能继续基于当前 Canvas 叠加。切换流版本
            // 并强制完整关键帧，让网页丢弃旧的异步 JPEG 解码任务后重新建立基线。
            firstFrame_ = true;
            streamKeyFrameRequired_ = true;
            if (encoder_ && encoder_->IsH264()) {
                encoder_->RequestKeyFrame();
            }
            resetFramePipelineReady("patch batch resync required");
            streamId_.fetch_add(1);
            server_.Broadcaster().BroadcastText(MakeCfgJson());
            log::Warn("patch batch resync consumed, requesting full-frame baseline");
            return true;
        };
        consumePatchResyncRequest();

        // H.264 流开始或重同步后必须等独立 IDR。若同一 MFT 在两个恢复窗口中
        // 都没有提供 IDR，继续丢弃 P 帧只会让浏览器永久黑屏：先重建一次 MFT，
        // 再失败则切到 JPEG，下一次分辨率/桌面重建仍会重新尝试 H.264。
        if (!streamH264_.load() || !streamKeyFrameRequired_) {
            h264KeyFrameWaitingSince = {};
            h264KeyFrameRecoveryAttempts = 0;
        } else if (h264KeyFrameWaitingSince.time_since_epoch().count() == 0) {
            h264KeyFrameWaitingSince = t0;
        } else if (t0 - h264KeyFrameWaitingSince >= kH264KeyFrameWatchdogInterval) {
            h264KeyFrameWaitingSince = t0;
            if (encoder_ && encoder_->IsH264() &&
                h264KeyFrameRecoveryAttempts < kH264KeyFrameMftRebuildLimit) {
                ++h264KeyFrameRecoveryAttempts;
                log::Warn("H.264 keyframe watchdog rebuilding MFT, attempt=" +
                          std::to_string(h264KeyFrameRecoveryAttempts));
                resetFramePipelineReady("H.264 keyframe watchdog rebuild");
                capture_.SetGpuOutputEnabled(false);
                encoder_->Release();
                encoder_.reset();
                encoderReady_ = false;
                encoderDeviceGeneration = 0;
                // 静态 DXGI 桌面也必须产生一张完整输入帧，才能重新初始化 MFT。
                firstFrame_ = true;
                continue;
            }
            if (encoder_ && encoder_->IsH264() && encoder_->ForceJpegFallback()) {
                resetFramePipelineReady("H.264 keyframe watchdog JPEG fallback");
                capture_.SetGpuOutputEnabled(false);
                streamH264_.store(false);
                streamH264Profile_.store(0x42E01E);
                streamKeyFrameRequired_ = false;
                firstFrame_ = true;
                encoderReady_ = true;
                streamId_.fetch_add(1);
                server_.Broadcaster().BroadcastText(MakeCfgJson());
                log::Warn("H.264 keyframe watchdog switched stream to JPEG");
                continue;
            }
        }

        // H.264 delta 之间有参考关系。已发未确认与待发送共用两帧端到端窗口，
        // 窗口占满时不再采集/编码下一张；直接跳过输入帧可保持编码器与浏览器的
        // 参考链连续，也避免多保留一张过时画面。首帧和请求恢复期间仍放行，使
        // 独立 IDR 能及时替换过期画面。
        const bool needsOrderedPatchCredit = patchCapability && !firstFrame_ &&
            !streamKeyFrameRequired_;
        if (needsOrderedPatchCredit && !server_.Broadcaster().WaitForPatchFrameCredit(
                std::chrono::milliseconds(std::max(1, targetMs)))) {
            continue;
        }
        if (!needsOrderedPatchCredit && streamH264_.load() && !streamKeyFrameRequired_ &&
            !server_.Broadcaster().WaitForH264FrameCredit(
                std::chrono::milliseconds(std::max(1, targetMs)))) {
            ++metrics.h264CreditWaits;
            ++adaptiveH264CreditWaits;
            continue;
        }
        // 信用等待期间也可能收到 ACK 超时。先消费请求并在下一轮从 IDR 开始，
        // 防止恰好被唤醒的一张旧 delta 越过发送队列。
        if (consumeH264ResyncRequest() || consumePatchResyncRequest()) {
            continue;
        }

        // DXGI 空闲时会阻塞等待桌面变化。等待一个目标帧周期即可在静态画面
        // 下避免轮询空转，同时不会再因固定 50ms 把 30FPS 限制为约 20FPS。
        const auto captureStartedAt = std::chrono::steady_clock::now();
        const bool requireFreshFrame = firstFrame_ || streamKeyFrameRequired_;
        // BitBlt 路径用稀疏指纹跳过静态画面，锁屏密码圆点、按钮高亮等小变化却
        // 可能恰好不落在采样点上，旧实现会延迟到一秒保底刷新才可见。只对已经
        // 成功注入的离散输入跳过去重；高频 move 不会走此标志，仍保持低 CPU。
        const bool forceGdiFrame =
            gdiFreshFrameRequested_.exchange(false, std::memory_order_acq_rel);
        const CaptureResult captureResult = capture_.CaptureFrame(
            frame, static_cast<DWORD>(std::max(1, targetMs)),
            requireFreshFrame || (capture_.IsUsingGdi() && forceGdiFrame));
        metrics.captureAttempts++;
        const uint64_t captureElapsedMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - captureStartedAt).count());
        metrics.captureTimeMs += captureElapsedMs;
        const bool sampledWithGdi = capture_.IsUsingGdi();
        // CaptureGDI 在返回 kNoChange 前也已经执行了完整 BitBlt/StretchBlt。这里
        // 必须立刻累计该耗时，不能等到“有编码帧”分支，否则 stream metrics 会低估
        // 锁屏静止、hover 小变化等最常见 GDI 场景的实际采集成本。
        if (frame.gdiBltUs != 0) {
            metrics.gdiBltUs += frame.gdiBltUs;
            ++metrics.gdiBltSamples;
        }
        if (sampledWithGdi && captureResult != CaptureResult::kFailed) {
            ++adaptiveGdiCaptureSamples;
            adaptiveGdiCaptureTimeMs += captureElapsedMs;
        }
        PointerUpdate pointer;
        if (capture_.TakePointerUpdate(pointer)) {
            // 指针使用独立小消息发送。DXGI 的 pointer-only 通知不再触发整帧 H.264
            // 编码，浏览器仍能立即看到远端鼠标位置。
            nlohmann::json cursor = {
                {"t", "cursor"},
                {"visible", pointer.visible},
                {"style", CursorStyleName(pointer.style)},
            };
            if (pointer.visible) {
                cursor["x"] = pointer.x;
                cursor["y"] = pointer.y;
            }
            server_.Broadcaster().BroadcastText(cursor.dump(), true);
        }
        if (captureResult == CaptureResult::kNoChange ||
            captureResult == CaptureResult::kPointerOnly) {
            consecutiveCaptureFailures = 0;
            if (captureResult == CaptureResult::kPointerOnly) {
                metrics.pointerOnlyFrames++;
            } else {
                metrics.noChangeFrames++;
            }
            const auto t1 = std::chrono::steady_clock::now();
            const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            const int remain = targetMs - static_cast<int>(used);
            if (remain > 0) {
                if (capture_.IsUsingGdi()) {
                    WaitForGdiCaptureDelay(remain);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(remain));
                }
            }
            continue;
        }
        if (captureResult == CaptureResult::kFailed) {
            metrics.captureFailures++;
            ++consecutiveCaptureFailures;
            const bool reinitialize = consecutiveCaptureFailures == 1 ||
                consecutiveCaptureFailures % kCaptureFailureReinitializeInterval == 0;
            if (reinitialize) {
                log::Warn("capture failure, rebuilding resources (consecutive=" +
                          std::to_string(consecutiveCaptureFailures) + ")");
                capture_.ResetForDesktop();
            }
            const int backoffMs = captureFailureBackoffMs(consecutiveCaptureFailures, targetMs);
            if (capture_.IsUsingGdi()) {
                WaitForGdiCaptureDelay(backoffMs);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            }
            continue;
        }
        consecutiveCaptureFailures = 0;
        metrics.capturedFrames++;
        metrics.capturedFrameTimeMs += captureElapsedMs;
        if (frame.IsDirectDxgi()) {
            metrics.directMappedFrames++;
        }
        if (frame.IsGpuNv12()) {
            metrics.gpuNv12Frames++;
            metrics.gpuNv12SubmissionUs += frame.gpuNv12SubmissionUs;
        }
        if (frame.cpuCopyOrScaleUs != 0) {
            metrics.cpuCopyOrScaleUs += frame.cpuCopyOrScaleUs;
            ++metrics.cpuCopyOrScaleSamples;
        }
        CapturedFrameLease frameLease(capture_, frame);

        const bool needsFreshFrame = firstFrame_;
        // DXGI 已经以 kNoChange/kPointerOnly 过滤静态桌面；GDI 则在原始 DIB 上
        // 完成一次采样过滤。不要在缩放后的帧上做第二次哈希：它会额外读取整张
        // CPU 帧，且采样碰撞会把已确认变化的画面推迟到下一次强制刷新。
        firstFrame_ = false;

        if (encoder_ && encoder_->CanEncodeD3D11() &&
            encoderDeviceGeneration != capture_.DeviceGeneration()) {
            // 选择显示器后 CaptureFrame 可在本轮内部重建 DXGI；同样不能把新
            // surface 喂给旧 device manager，下一段会立即按当前分辨率重建编码器。
            log::Info("DXGI device changed, reinitializing H.264 encoder");
            encoder_->Release();
            encoder_.reset();
            encoderReady_ = false;
            encoderDeviceGeneration = 0;
        }

        if (!encoder_ || frame.width != deskWidth_.load() || frame.height != deskHeight_.load()) {
            resetFramePipelineReady("encoder reinitializing");
            deskWidth_.store(frame.width);
            deskHeight_.store(frame.height);
            encoderReady_ = false;
            capture_.SetGpuOutputEnabled(false);
            encoder_ = std::make_unique<EncoderMf>();
            if (!encoder_->Init(frame.width, frame.height, cfg_.fps, adaptiveBitrate,
                                capture_.D3DDevice())) {
                log::Error("encoder init failed for " + std::to_string(frame.width) +
                           "x" + std::to_string(frame.height));
                encoder_.reset();
                frameLease.Release();
                // 若 DXGI 桌面已静止，firstFrame=false 会让后续 CaptureFrame 永远
                // 返回 kNoChange，编码器也就没有机会再次初始化，表现为永久黑屏。
                firstFrame_ = true;
                // GDI 空闲路径依靠进程内唤醒事件降低锁屏 BitBlt 的常驻负载；编码器
                // 初始化失败也必须保留同一等待语义，避免远端输入到达后仍被固定 sleep
                // 延后一个完整空闲周期。
                if (capture_.IsUsingGdi()) {
                    WaitForGdiCaptureDelay(targetMs);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(targetMs));
                }
                continue;
            }
            encoderReady_ = true;
            streamH264_.store(encoder_->IsH264());
            streamH264Profile_.store(encoder_->H264CodecProfile());
            streamKeyFrameRequired_ = encoder_->IsH264();
            encoderDeviceGeneration = capture_.DeviceGeneration();
            capture_.SetGpuOutputEnabled(encoder_->CanEncodeD3D11());
            // 尺寸/编码器变化后提升流版本并重新下发 cfg。浏览器异步图片解码可能
            // 在 cfg 到达后才完成，因此二进制帧会带上该版本供控制端判定。
            streamId_.fetch_add(1);
            server_.Broadcaster().BroadcastText(MakeCfgJson());
        }

        bool patchQueued = false;
        if (patchCapability && !needsFreshFrame && encoderReady_ && !frame.Empty()) {
            std::vector<JpegTile> tiles;
            if (BuildJpegPatchTiles(frame, patchThreshold, *encoder_, tiles)) {
                uint64_t patchBytes = 0;
                for (const auto& tile : tiles) {
                    patchBytes += tile.data.size();
                }
                const uint64_t patchTimestampUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const FrameQueueResult patchResult = server_.Broadcaster().BroadcastPatch(
                    std::move(tiles), streamId_.load(), patchTimestampUs);
                if (patchResult == FrameQueueResult::kQueued) {
                    patchQueued = true;
                    ++metrics.encodedFrames;
                    metrics.encodedBytes += patchBytes;
                    if (!framePipelineReady) {
                        framePipelineReady = true;
                        if (frameReadyEvent_ && !stop_.load()) {
                            SetEvent(frameReadyEvent_);
                        }
                    }
                } else if (patchResult == FrameQueueResult::kPatchResyncRequired) {
                    firstFrame_ = true;
                    streamKeyFrameRequired_ = true;
                    if (encoder_->IsH264()) {
                        encoder_->RequestKeyFrame();
                    }
                    resetFramePipelineReady("patch queue resync required");
                    patchQueued = true;
                }
            }
        }

        if (!patchQueued && encoderReady_ && !frame.Empty()) {
            std::vector<EncodedChunk> chunks;
            const auto encodeStartedAt = std::chrono::steady_clock::now();
            const bool wasH264 = streamH264_.load();
            if (needsFreshFrame && encoder_->IsH264()) {
                encoder_->RequestKeyFrame();
            }
            const bool gpuFrame = frame.IsGpuNv12();
            const bool encoded = gpuFrame
                ? encoder_->EncodeD3D11(frame.nv12Texture.Get(), chunks)
                : encoder_->Encode(frame.Pixels(), frame.StrideBytes(), chunks);
            const EncoderFrameTiming& encoderTiming = encoder_->LastFrameTiming();
            if (encoderTiming.bgraToNv12Us != 0) {
                metrics.bgraToNv12Us += encoderTiming.bgraToNv12Us;
                ++metrics.bgraToNv12Samples;
            }
            if (encoderTiming.mfInputPreparationUs != 0) {
                metrics.mfInputPreparationUs += encoderTiming.mfInputPreparationUs;
                ++metrics.mfInputPreparationSamples;
            }
            if (encoderTiming.d3dInputWrapUs != 0) {
                metrics.d3dInputWrapUs += encoderTiming.d3dInputWrapUs;
                ++metrics.d3dInputWrapSamples;
            }
            const uint64_t encodeElapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - encodeStartedAt).count());
            metrics.encodeAttempts++;
            metrics.encodeTimeMs += encodeElapsedMs;
            ++adaptiveEncodeAttempts;
            adaptiveEncodeTimeMs += encodeElapsedMs;
            if (encoded) {
                if (wasH264 != encoder_->IsH264()) {
                    resetFramePipelineReady("encoder codec changed");
                    streamH264_.store(encoder_->IsH264());
                    streamH264Profile_.store(encoder_->H264CodecProfile());
                    streamKeyFrameRequired_ = encoder_->IsH264();
                    capture_.SetGpuOutputEnabled(encoder_->CanEncodeD3D11());
                    streamId_.fetch_add(1);
                    server_.Broadcaster().BroadcastText(MakeCfgJson());
                }
                if (encoder_->IsH264()) {
                    const uint32_t actualProfile = encoder_->H264CodecProfile();
                    if (streamH264Profile_.load() != actualProfile) {
                        resetFramePipelineReady("H.264 profile changed");
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
                                                              c.isKey, c.timestampUs, h264,
                                                              patchCapability);
                    if (!framePipelineReady &&
                        (queueResult == FrameQueueResult::kQueued ||
                         queueResult == FrameQueueResult::kReplaced)) {
                        framePipelineReady = true;
                        if (frameReadyEvent_ && !stop_.load()) {
                            SetEvent(frameReadyEvent_);
                        }
                        log::Info("capture pipeline ready: first encoded frame queued");
                    }
                    if (queueResult == FrameQueueResult::kH264ResyncRequired) {
                        // 待发送 delta 已无法与下一帧构成连续 GOP；必须从新的
                        // IDR 恢复，不能沿用 JPEG 的“最新帧覆盖”策略。
                        resetFramePipelineReady("H.264 resync required");
                        streamKeyFrameRequired_ = true;
                        encoder_->RequestKeyFrame();
                        log::Warn("H.264 pending delta dropped, requesting IDR resync");
                        break;
                    }
                    if (queueResult == FrameQueueResult::kPatchResyncRequired) {
                        firstFrame_ = true;
                        streamKeyFrameRequired_ = true;
                        if (encoder_->IsH264()) {
                            encoder_->RequestKeyFrame();
                        }
                        resetFramePipelineReady("full frame collided with patch batch");
                        log::Warn("full frame deferred until patch resync baseline");
                        break;
                    }
                }
            } else {
                metrics.encodeFailures++;
                resetFramePipelineReady("encoding failed");
                if (gpuFrame) {
                    // GPU surface 被驱动/MFT 拒绝时不能把 NV12 误交给 JPEG 回退。
                    // 释放此编码器并关闭 surface 输出，下一轮自动回到已验证的 CPU
                    // BGRA->NV12 路径重新建流。
                    log::Warn("H.264 D3D11 input failed, reverting to CPU encoding");
                    capture_.DisableGpuOutputForCurrentDevice();
                    encoder_->Release();
                    encoder_.reset();
                    encoderReady_ = false;
                    streamKeyFrameRequired_ = true;
                    encoderDeviceGeneration = 0;
                    // GPU 输入失败后的 CPU 回退同样需要一张完整基线；不能依赖
                    // 静态桌面恰好发生变化来触发下一次编码器初始化。
                    firstFrame_ = true;
                }
            }
        }

        // 不要让 staging texture 在帧率节流 sleep 期间保持 Map，下一轮可立即复用。
        frameLease.Release();
        const auto t1 = std::chrono::steady_clock::now();
        const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const int remain = targetMs - static_cast<int>(used);
        if (remain > 0) {
            if (capture_.IsUsingGdi()) {
                WaitForGdiCaptureDelay(remain);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(remain));
            }
        }
    }
    // Media Foundation 的启动与关闭必须保持在同一线程。硬件 MFT 还可能持有该
    // 线程关联的 COM 资源，因此必须在 CoUninitialize 前释放编码器。
    if (encoder_) {
        encoder_->Release();
        encoder_.reset();
        encoderReady_ = false;
    }
    if (comInitialized) {
        CoUninitialize();
    }
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
    int qualityValue = requestedStreamQuality_.load(std::memory_order_relaxed);
    if (!IsStreamQualityValid(qualityValue)) {
        qualityValue = static_cast<int>(StreamQuality::kAutomatic);
    }
    j["quality"] = StreamQualityName(static_cast<StreamQuality>(qualityValue));
    j["patch_threshold"] = requestedPatchThreshold_.load(std::memory_order_relaxed);
    j["patches"] = requestedPatchCapability_.load(std::memory_order_relaxed);
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
        int decodeMsTotal = 0;
        int decodeMsSamples = 0;
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
                                maxWsBuffered) ||
            !ReadOptionalJsonIntInRange(j, "decode_ms_total", 0, 10'000'000,
                                        decodeMsTotal) ||
            !ReadOptionalJsonIntInRange(j, "decode_ms_samples", 0, 10'000,
                                        decodeMsSamples)) {
            return;
        }
        clientDrawnFrames_.fetch_add(static_cast<uint64_t>(drawn), std::memory_order_relaxed);
        clientDroppedFrames_.fetch_add(static_cast<uint64_t>(dropped),
                                       std::memory_order_relaxed);
        clientDrawMsTotal_.fetch_add(static_cast<uint64_t>(drawMsTotal),
                                     std::memory_order_relaxed);
        clientDrawMsSamples_.fetch_add(static_cast<uint64_t>(drawMsSamples),
                                       std::memory_order_relaxed);
        clientDecodeMsTotal_.fetch_add(static_cast<uint64_t>(decodeMsTotal),
                                       std::memory_order_relaxed);
        clientDecodeMsSamples_.fetch_add(static_cast<uint64_t>(decodeMsSamples),
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
        WakeGdiCaptureLoop();
        return;
    }
    if (t == "stream") {
        std::string qualityText;
        int threshold = 0;
        bool patches = false;
        StreamQuality quality = StreamQuality::kAutomatic;
        if (!ReadJsonString(j, "quality", qualityText) ||
            !ParseStreamQuality(qualityText, quality) ||
            !ReadJsonIntInRange(j, "patch_threshold", kMinPatchThresholdPercent,
                                kMaxPatchThresholdPercent, threshold) ||
            !IsPatchThresholdValid(threshold) || !ReadJsonBool(j, "patches", patches)) {
            return;
        }
        requestedStreamQuality_.store(static_cast<int>(quality), std::memory_order_relaxed);
        requestedPatchThreshold_.store(threshold, std::memory_order_relaxed);
        requestedPatchCapability_.store(patches, std::memory_order_relaxed);
        streamPreferenceGeneration_.fetch_add(1, std::memory_order_release);
        WakeGdiCaptureLoop(true);
        return;
    }
    if (t == "monitor") {
        int index = -1;
        if (!ReadJsonIntInRange(j, "index", -1, 1024, index)) {
            return;
        }
        capture_.SetMonitor(index);
        frameResetRequested_.store(true);
        WakeGdiCaptureLoop();
        log::Info("monitor switched to idx=" + std::to_string(capture_.SelectedMonitor()));
        return;
    }
    if (t != "key" && t != "mouse" && t != "move" && t != "wheel") {
        return;
    }

    // WebSocket 回调在线程池工作线程中执行。每个线程独立绑定当前输入桌面，
    // 避免把采集线程的 desktop handle 跨线程复用。键盘、按键鼠标和滚轮会立即
    // 核验 desktop，防止锁屏/解锁后一秒的轮询窗口把关键输入送进旧桌面；纯移动
    // 事件仍按秒检查，避免高频 OpenInputDesktop 增加系统调用压力。一旦核验失败，
    // 所有事件（包括 move）都暂停，直到本线程重新确认当前输入桌面。
    thread_local DesktopAccess inputDesktop;
    thread_local auto nextDesktopCheck = std::chrono::steady_clock::now();
    thread_local bool inputDesktopVerified = false;
    thread_local bool inputDesktopFailureLogged = false;
    const auto now = std::chrono::steady_clock::now();
    const bool criticalInput = t != "move";
    if (!inputDesktop.IsBound()) {
        if (!inputDesktop.Bind()) {
            return;
        }
        inputDesktopVerified = true;
        inputDesktopFailureLogged = false;
        nextDesktopCheck = now + std::chrono::seconds(1);
    } else if (!inputDesktopVerified || criticalInput || now >= nextDesktopCheck) {
        if (!criticalInput) {
            nextDesktopCheck = now + std::chrono::seconds(1);
        }
        const DesktopRebindResult rebindResult = inputDesktop.CheckRebind();
        if (rebindResult == DesktopRebindResult::kFailed) {
            inputDesktopVerified = false;
            if (!inputDesktopFailureLogged) {
                log::Warn("remote input paused: unable to verify current input desktop");
                inputDesktopFailureLogged = true;
            }
            return;
        }
        if (!inputDesktopVerified && inputDesktopFailureLogged) {
            log::Info("remote input desktop verification recovered");
        }
        inputDesktopVerified = true;
        inputDesktopFailureLogged = false;
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
        lastRemoteInputTick_.store(static_cast<uint64_t>(GetTickCount64()),
                                   std::memory_order_relaxed);
        if (Input::SendKey(static_cast<USHORT>(scanCode), down, extended)) {
            WakeGdiCaptureLoop(true);
        }
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
        lastRemoteInputTick_.store(static_cast<uint64_t>(GetTickCount64()),
                                   std::memory_order_relaxed);
        const bool moved = Input::SendMouseAbs(virtualX, virtualY);
        const bool buttonSent = Input::SendMouseButton(button, down);
        if (moved || buttonSent) {
            WakeGdiCaptureLoop(buttonSent);
        }
    } else if (t == "move") {
        double x = 0.0;
        double y = 0.0;
        if (!ReadJsonFiniteNumber(j, "x", x) || !ReadJsonFiniteNumber(j, "y", y)) {
            return;
        }
        double virtualX = 0.0;
        double virtualY = 0.0;
        if (capture_.MapNormalizedToVirtual(x, y, virtualX, virtualY)) {
            if (!Input::SendMouseAbs(virtualX, virtualY)) {
                return;
            }
            const uint64_t nowTick = static_cast<uint64_t>(GetTickCount64());
            lastRemoteInputTick_.store(nowTick, std::memory_order_relaxed);
            // 浏览器原生指针会立即跟随本地位置，CaptureLoop 则按已提升的交互 FPS
            // 同步远端实际光标样式。仅以初始 GDI 交互帧率打断等待：首个移动不会
            // 被 2 FPS 空闲周期拖慢，持续移动也不会以 60Hz 以上完整 BitBlt。
            uint64_t previousWakeTick = lastGdiMoveWakeTick_.load(std::memory_order_relaxed);
            while (nowTick - previousWakeTick >= kGdiMoveWakeIntervalMs) {
                if (lastGdiMoveWakeTick_.compare_exchange_weak(
                        previousWakeTick, nowTick, std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    WakeGdiCaptureLoop();
                    break;
                }
            }
        }
    } else if (t == "wheel") {
        int delta = 0;
        if (!ReadJsonIntInRange(j, "delta", -1200, 1200, delta)) {
            return;
        }
        lastRemoteInputTick_.store(static_cast<uint64_t>(GetTickCount64()),
                                   std::memory_order_relaxed);
        if (Input::SendWheel(delta)) {
            WakeGdiCaptureLoop(true);
        }
    }
}

void Agent::OnControllerDisconnected() {
    // 该回调在接收输入的 WebSocket 工作线程中运行。断连恰好发生在锁屏/解锁
    // 切换后时，该线程可能仍绑定旧 desktop；先绑定当前 input desktop，确保释放
    // 的键鼠事件送往用户当前可见的界面。
    // 先置位再尝试同步释放；若当前窗口无法绑定，CaptureLoop 会在下一次成功校验
    // input desktop 后消费该请求，确保不会遗留按住的 Ctrl/Shift 或鼠标按钮。
    // 先关住新的控制端槽位，再进行可能跨桌面重试的释放。CaptureLoop 与本线程
    // 任意先后完成时，ControllerHandoff 都会同时等待“handler 脱离”和“输入释放”。
    server_.Broadcaster().BeginControllerInputCleanup();
    inputReleaseRequested_.store(true, std::memory_order_release);
    DesktopAccess disconnectDesktop;
    if (!disconnectDesktop.Bind()) {
        log::Warn("controller disconnect: unable to bind current input desktop");
    } else if (Input::ReleaseAll()) {
        if (inputReleaseRequested_.exchange(false, std::memory_order_acq_rel)) {
            server_.Broadcaster().CompleteControllerInputCleanup();
        }
    } else {
        // Input 会保留失败项；CaptureLoop 取得当前 desktop 后会再次发送 keyup/mouseup。
        log::Warn("controller disconnect: input release incomplete, deferring retry");
    }
    // 即使控制端快速重连，以 CaptureLoop 未观察到“零客户端”为例，也要确保
    // 下一帧从 IDR 开始，并恢复下一位控制者的质量上限。
    frameResetRequested_.store(true);
    qualityResetRequested_.store(true);
    // 网页偏好属于当前控制端。断开后立刻恢复默认，避免下一位控制端在旧浏览器
    // 尚未完成鉴权/能力协商时继承原始分辨率或图块协议。
    requestedStreamQuality_.store(static_cast<int>(StreamQuality::kAutomatic),
                                  std::memory_order_relaxed);
    requestedPatchThreshold_.store(kDefaultPatchThresholdPercent, std::memory_order_relaxed);
    requestedPatchCapability_.store(false, std::memory_order_relaxed);
    streamPreferenceGeneration_.fetch_add(1, std::memory_order_release);
}

}  // namespace remote_assist
