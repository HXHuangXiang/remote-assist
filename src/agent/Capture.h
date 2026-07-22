#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace remote_assist {

struct CapturedFrame {
    int width = 0;
    int height = 0;
    // GDI 与缩放后的 DXGI 路径持有完整 CPU 副本；未缩放 DXGI 路径直接借用已映射
    // staging texture，避免再复制一遍 BGRA。该借用仅在 Capture::ReleaseFrame 前有效。
    std::vector<uint8_t> data;
    const uint8_t* mappedPixels = nullptr;
    size_t mappedStrideBytes = 0;
    // DXGI + 视频处理器路径下直接交给硬件 H.264 MFT 的 NV12 surface。该资源
    // 与 data/mappedPixels 互斥，生命周期同样截至 Capture::ReleaseFrame。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Texture;

    bool IsDirectDxgi() const { return mappedPixels != nullptr; }
    bool IsGpuNv12() const { return nv12Texture.Get() != nullptr; }
    bool Empty() const {
        return IsGpuNv12() || IsDirectDxgi() ? width <= 0 || height <= 0 : data.empty();
    }
    const uint8_t* Pixels() const { return IsDirectDxgi() ? mappedPixels : data.data(); }
    size_t StrideBytes() const {
        return IsDirectDxgi() ? mappedStrideBytes : static_cast<size_t>(width) * 4;
    }
};

// 采集结果区分“无变化”和真正失败，避免 DXGI 空闲超时退化为 GDI 全屏抓取。
enum class CaptureResult {
    kFrame,
    kNoChange,
    // DXGI Desktop Duplication 会因指针位置/形状变化返回上一张桌面图像。指针位置
    // 会由独立的 cursor 协议下发，无需对相同像素再次回读、编码和发送。
    kPointerOnly,
    kFailed,
};

// 指针坐标相对于当前视频画面归一化。当前版本在浏览器端叠加低成本通用箭头，避免
// Desktop Duplication 将指针更新误当成整帧桌面变化，后续可在此基础上补充原生形状。
struct PointerUpdate {
    bool visible = false;
    double x = 0.0;
    double y = 0.0;
};

// 显示器信息(矩形坐标相对于虚拟屏幕原点)。
struct MonitorInfo {
    int index = 0;
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
};

class Capture {
public:
    Capture() = default;
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool Init();
    // waitMs 是 DXGI 等待桌面变化的上限；由 Agent 按实际目标帧率传入，避免
    // 固定超时把高帧率配置限制在约 20 FPS。requireFreshFrame 用于新控制端或
    // 解码恢复：DXGI 没有桌面变更时也必须提供一张完整首帧，内部会一次性回退 GDI。
    CaptureResult CaptureFrame(CapturedFrame& out, DWORD waitMs, bool requireFreshFrame);
    // 结束直接映射的 DXGI 帧借用。编码完成后必须调用；对普通 CPU 帧是无操作。
    void ReleaseFrame(CapturedFrame& frame);
    // 取得自上次调用后最新的指针可见性/位置变化。仅由 CaptureLoop 调用。
    bool TakePointerUpdate(PointerUpdate& out);

    // 输入桌面切换后释放与旧桌面关联的采集资源并重新初始化。
    void ResetForDesktop();

    // 多显示器:枚举显示器列表。-1=全部(虚拟屏幕),>=0=指定显示器。
    // 重新枚举显示器并按稳定设备顺序编号，保留当前选中的设备（若仍存在）。返回
    // true 表示布局、分辨率或选择项发生了变化，调用方应重建采集/编码资源并推送 cfg。
    bool EnumMonitors();
    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM);
    // 返回稳定副本，避免 WebSocket 线程生成 cfg 时与采集线程的热插拔重枚举竞争。
    std::vector<MonitorInfo> MonitorsSnapshot() const;
    void SetMonitor(int index);
    int SelectedMonitor() const { return selectedMonitor_.load(); }
    // 仅由采集线程调用。慢链路时降低输出上限可同时缩短缩放、编码和网络发送时间；
    // 源画面宽高均未超过上限时仍保持原始分辨率。
    void SetMaxOutputSize(int width, int height);
    // H.264 MFT 确认支持同一 D3D11 设备后启用。GDI、缩放和能力不完整的机器
    // 会继续走现有 CPU 帧路径。当前 D3D11 设备已确认失败时，此调用不会再次
    // 打开 GPU 输出，直到 Desktop Duplication 重建出新的 device generation。
    void SetGpuOutputEnabled(bool enabled);
    // 视频处理器或 H.264 MFT 拒绝 GPU surface 时调用。按 device generation
    // 熔断，避免每帧在 GPU 与 CPU 回退之间反复初始化、黑屏或刷日志。
    void DisableGpuOutputForCurrentDevice();
    bool IsGpuOutputEnabled() const { return gpuOutputEnabled_; }
    ID3D11Device* D3DDevice() const { return d3d_.Get(); }
    uint64_t DeviceGeneration() const { return deviceGeneration_; }
    // GDI 包含锁屏、DXGI 失效、跨 adapter 多屏与旋转屏等回退路径，采集成本显著
    // 高于单输出或同 adapter 的 DXGI 合成路径。
    // 仅由采集线程调用，因此无需额外同步。
    bool IsUsingGdi() const { return use_gdi_; }

    // 将当前画面中的归一化坐标映射为整个虚拟桌面的归一化坐标。
    // 选择单屏时会加上该显示器在虚拟桌面中的偏移。
    bool MapNormalizedToVirtual(double x, double y, double& virtualX, double& virtualY) const;

private:
    struct DxgiOutputCapture {
        Microsoft::WRL::ComPtr<IDXGIOutput1> output;
        Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
        // Desktop Duplication 返回的 resource 在 ReleaseFrame 后即失效；多屏合成
        // 保存每个输出的最近完整副本，以便只更新发生变化的那一块。
        Microsoft::WRL::ComPtr<ID3D11Texture2D> latestFrame;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool hasFrame = false;
    };

    bool InitDXGI();
    // “全部屏幕”且所有输出位于同一 D3D adapter 时，分别 duplicate 后在 GPU
    // 合成虚拟桌面；跨 adapter、旋转屏或不完整输出集合继续安全回退 GDI。
    bool InitDXGIComposite(const std::vector<MonitorInfo>& monitors);
    CaptureResult CaptureDXGI(CapturedFrame& out, DWORD waitMs);
    CaptureResult CaptureDXGIComposite(CapturedFrame& out, DWORD waitMs);
    CaptureResult CaptureGDI(CapturedFrame& out, bool forceFrame);
    bool CreateGpuNv12Frame(ID3D11Texture2D* source, int width, int height,
                            CapturedFrame& out);
    bool EnsureVideoProcessor(int sourceWidth, int sourceHeight,
                              int outputWidth, int outputHeight);
    void CopyRegionToFrame(const uint8_t* source, size_t sourceStrideBytes,
                           int x, int y, int width, int height,
                           CapturedFrame& out);
    void ReleaseAll();
    void UpdatePointerFromDesktop(bool visible, int screenX, int screenY);
    void UpdatePointerFromFrame(const DXGI_OUTDUPL_FRAME_INFO& frameInfo);
    void UpdatePointerFromSystem();
    void RecordGdiCaptureFailure(DWORD error);
    void RecordGdiCaptureRecovery();

    // DXGI
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> composite_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> compositeRtv_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    bool stagingMapped_ = false;
    bool dxgiComposite_ = false;
    // 多输出刚建立时若只收到其中一块的首帧，缓存尚不能组成完整画面。该标志
    // 请求一次 GDI 快照填补控制端画面，但不会在静态桌面中每帧退回 GDI。
    bool compositeNeedsGdiFrame_ = false;
    int compositeWidth_ = 0;
    int compositeHeight_ = 0;
    std::vector<DxgiOutputCapture> dxgiOutputs_;
    bool gpuOutputEnabled_ = false;
    // 记录当前设备是否已发生过 GPU 路径故障。0 表示尚未初始化 DXGI device；
    // InitDXGI 成功时 generation 单调递增，因此旧设备的失败不会阻断新设备重试。
    uint64_t gpuOutputDisabledGeneration_ = 0;
    int videoProcessorSourceWidth_ = 0;
    int videoProcessorSourceHeight_ = 0;
    int videoProcessorOutputWidth_ = 0;
    int videoProcessorOutputHeight_ = 0;
    uint64_t deviceGeneration_ = 0;

    // GDI
    HDC gdi_dc_ = nullptr;
    bool gdi_dc_from_getdc_ = false;
    HDC mem_dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HBITMAP old_bmp_ = nullptr;
    void* bits_ = nullptr;

    int dibW_ = 0;   // DIB 宽(虚拟屏幕宽)
    int dibH_ = 0;   // DIB 高(虚拟屏幕高)
    // GDI/BitBlt 不提供 DXGI Desktop Duplication 那样的“无变化”通知。对原始
    // DIB 采样哈希可在静态锁屏/多屏画面时跳过昂贵的缩放与编码；每秒仍强制
    // 输出一次完整帧，避免极小变化恰好未落入采样点而长期不可见。
    uint64_t gdiFingerprint_ = 0;
    bool hasGdiFingerprint_ = false;
    std::chrono::steady_clock::time_point lastGdiFullFrameAt_{};
    uint64_t gdiConsecutiveFailures_ = 0;
    std::chrono::steady_clock::time_point lastGdiFailureLogAt_{};

    // 指针状态与画面只由采集线程访问，无需同步。pointerDirty_ 允许 pointer-only
    // DXGI 通知通过独立 WebSocket 消息立即到达浏览器，而不会触发整帧编码。
    PointerUpdate pointer_{};
    bool pointerKnown_ = false;
    bool pointerDirty_ = false;

    // 多显示器
    mutable std::mutex monitorMu_;
    std::vector<MonitorInfo> monitors_;
    std::atomic<int> selectedMonitor_{-1};  // -1=全部
    // 当前采集资源对应的选择项。-2 表示尚未初始化；即使 DXGI 不可用也记录，
    // 防止 GDI 回退路径在每一帧重复重建资源。
    int activeMonitor_ = -2;

    int width_ = 0;
    int height_ = 0;
    int maxOutputWidth_ = 1920;
    int maxOutputHeight_ = 1080;
    bool use_gdi_ = false;

    // 缩放时的相对源坐标表。分辨率稳定时重复使用，避免每帧按像素执行整数除法。
    int scaleMapSourceWidth_ = 0;
    int scaleMapSourceHeight_ = 0;
    int scaleMapOutputWidth_ = 0;
    int scaleMapOutputHeight_ = 0;
    std::vector<int> scaleMapX_;
    std::vector<int> scaleMapY_;
};

}  // namespace remote_assist
