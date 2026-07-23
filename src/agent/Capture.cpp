#include "agent/Capture.h"

#include "common/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

namespace remote_assist {

namespace {

// 每行错开采样相位，避免固定列的细小变化一直漏检。调用方同时比对四个相位组，
// 每组只与自身的历史摘要比较；合计采样约 1/4 像素，仍远低于 GDI BitBlt 与
// BGRA->NV12 的整帧内存流量，却能在当前交互帧内发现任一相位覆盖的小区域变化。
uint64_t FingerprintBgraRegion(const uint8_t* source, size_t sourceStrideBytes,
                               int x, int y, int width, int height,
                               size_t phaseOffsetBytes) {
    constexpr size_t kSampleStrideBytes = 64;
    uint64_t hash = 1469598103934665603ULL;
    const size_t regionBytes = static_cast<size_t>(width) * 4;
    for (int row = 0; row < height; ++row) {
        const uint8_t* sourceRow = source + static_cast<size_t>(y + row) * sourceStrideBytes +
            static_cast<size_t>(x) * 4;
        const size_t phase = (phaseOffsetBytes + static_cast<size_t>(row) * 20) %
            kSampleStrideBytes;
        for (size_t offset = phase; offset + sizeof(uint32_t) <= regionBytes;
             offset += kSampleStrideBytes) {
            uint32_t pixel = 0;
            std::memcpy(&pixel, sourceRow + offset, sizeof(pixel));
            hash ^= pixel;
            hash *= 1099511628211ULL;
        }
    }
    return hash ^ (static_cast<uint64_t>(width) << 32) ^ static_cast<uint32_t>(height);
}

bool SameMonitorLayout(const std::vector<MonitorInfo>& left,
                       const std::vector<MonitorInfo>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.name != b.name || a.x != b.x || a.y != b.y ||
            a.w != b.w || a.h != b.h) {
            return false;
        }
    }
    return true;
}

}  // namespace

// EnumDisplayMonitors 回调:收集每个显示器信息。
BOOL CALLBACK Capture::MonitorEnumProc(HMONITOR hMon, HDC, LPRECT lprcMonitor, LPARAM dwData) {
    auto* self = reinterpret_cast<Capture*>(dwData);
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorInfo m;
        m.index = static_cast<int>(self->monitors_.size());
        // 显示器名称:取设备名前 20 字符。
        char name[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);
        m.name = name;
        // 坐标转换为相对于虚拟屏幕原点。
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        m.x = lprcMonitor->left - vx;
        m.y = lprcMonitor->top - vy;
        m.w = lprcMonitor->right - lprcMonitor->left;
        m.h = lprcMonitor->bottom - lprcMonitor->top;
        // EnumMonitors 在持有 monitorMu_ 时发起该同步回调，因此可安全写入当前枚举列表。
        self->monitors_.push_back(m);
    }
    return TRUE;
}

Capture::~Capture() { ReleaseAll(); }

void Capture::ReleaseAll() {
    if (stagingMapped_ && ctx_.Get() && staging_.Get()) {
        ctx_->Unmap(staging_.Get(), 0);
        stagingMapped_ = false;
    }
    staging_.Reset();
    compositeRtv_.Reset();
    composite_.Reset();
    dxgiOutputs_.clear();
    dxgiComposite_ = false;
    compositeNeedsGdiFrame_ = false;
    compositeWidth_ = 0;
    compositeHeight_ = 0;
    videoProcessor_.Reset();
    videoProcessorEnumerator_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    dup_.Reset();
    output_.Reset();
    ctx_.Reset();
    d3d_.Reset();
    if (bmp_) {
        if (mem_dc_) SelectObject(mem_dc_, old_bmp_);
        DeleteObject(bmp_);
        bmp_ = nullptr;
    }
    if (mem_dc_) { DeleteDC(mem_dc_); mem_dc_ = nullptr; }
    if (gdi_dc_) {
        if (gdi_dc_from_getdc_) {
            ReleaseDC(nullptr, gdi_dc_);
        } else {
            DeleteDC(gdi_dc_);
        }
        gdi_dc_ = nullptr;
        gdi_dc_from_getdc_ = false;
    }
    bits_ = nullptr;
    dibW_ = 0;
    dibH_ = 0;
    gdiFingerprints_.fill(0);
    hasGdiFingerprint_.fill(false);
    lastGdiFullFrameAt_ = {};
    gdiPreviousFrame_.clear();
    gdiConsecutiveFailures_ = 0;
    lastGdiFailureLogAt_ = {};
    pointer_ = {};
    pointerKnown_ = false;
    pointerDirty_ = false;
    gpuOutputEnabled_ = false;
    videoProcessorSourceWidth_ = 0;
    videoProcessorSourceHeight_ = 0;
    videoProcessorOutputWidth_ = 0;
    videoProcessorOutputHeight_ = 0;
}

void Capture::ReleaseFrame(CapturedFrame& frame) {
    if (stagingMapped_ && ctx_.Get() && staging_.Get()) {
        ctx_->Unmap(staging_.Get(), 0);
        stagingMapped_ = false;
    }
    frame.mappedPixels = nullptr;
    frame.mappedStrideBytes = 0;
    frame.borrowedGdiPixels = false;
    frame.nv12Texture.Reset();
    frame.gpuNv12SubmissionUs = 0;
    frame.hasDirtyRegions = false;
    frame.dirtyRegions.clear();
}

bool Capture::TakePointerUpdate(PointerUpdate& out) {
    if (!pointerDirty_) {
        return false;
    }
    out = pointer_;
    pointerDirty_ = false;
    return true;
}

PointerCursorStyle Capture::CursorStyleFromHandle(HCURSOR cursor) {
    if (!cursor) {
        return PointerCursorStyle::kDefault;
    }
    // LoadCursor 返回共享的系统资源句柄，无需释放。比较标准句柄能覆盖文本、等待、
    // 手型和常用缩放等高频交互状态；应用自定义光标则保守显示默认箭头。
    if (cursor == LoadCursorW(nullptr, IDC_IBEAM)) return PointerCursorStyle::kText;
    if (cursor == LoadCursorW(nullptr, IDC_WAIT)) return PointerCursorStyle::kWait;
    if (cursor == LoadCursorW(nullptr, IDC_CROSS)) return PointerCursorStyle::kCrosshair;
    if (cursor == LoadCursorW(nullptr, IDC_HAND)) return PointerCursorStyle::kPointer;
    if (cursor == LoadCursorW(nullptr, IDC_SIZEALL) ||
        cursor == LoadCursorW(nullptr, IDC_SIZE)) return PointerCursorStyle::kMove;
    if (cursor == LoadCursorW(nullptr, IDC_SIZEWE)) return PointerCursorStyle::kEastWestResize;
    if (cursor == LoadCursorW(nullptr, IDC_SIZENS)) return PointerCursorStyle::kNorthSouthResize;
    if (cursor == LoadCursorW(nullptr, IDC_SIZENWSE)) {
        return PointerCursorStyle::kNorthwestSoutheastResize;
    }
    if (cursor == LoadCursorW(nullptr, IDC_SIZENESW)) {
        return PointerCursorStyle::kNortheastSouthwestResize;
    }
    if (cursor == LoadCursorW(nullptr, IDC_NO)) return PointerCursorStyle::kNotAllowed;
    if (cursor == LoadCursorW(nullptr, IDC_APPSTARTING)) return PointerCursorStyle::kProgress;
    if (cursor == LoadCursorW(nullptr, IDC_HELP)) return PointerCursorStyle::kHelp;
    return PointerCursorStyle::kDefault;
}

void Capture::UpdatePointerFromDesktop(bool visible, int screenX, int screenY,
                                       PointerCursorStyle style) {
    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 0 || virtualHeight <= 0) {
        return;
    }

    // 与采集、输入使用同一“相对虚拟桌面原点”坐标系。单屏模式中，指针离开被
    // 选择的显示器时明确下发 hidden，避免浏览器把旧箭头遗留在画面上。
    const int relativeX = screenX - virtualLeft;
    const int relativeY = screenY - virtualTop;
    int offsetX = 0;
    int offsetY = 0;
    int outputWidth = virtualWidth;
    int outputHeight = virtualHeight;
    const std::vector<MonitorInfo> monitors = MonitorsSnapshot();
    const int selectedMonitor = selectedMonitor_.load();
    if (selectedMonitor >= 0 && selectedMonitor < static_cast<int>(monitors.size())) {
        const auto& monitor = monitors[selectedMonitor];
        offsetX = monitor.x;
        offsetY = monitor.y;
        outputWidth = monitor.w;
        outputHeight = monitor.h;
    }
    const bool insideOutput = relativeX >= offsetX && relativeY >= offsetY &&
        relativeX < offsetX + outputWidth && relativeY < offsetY + outputHeight;
    PointerUpdate next;
    next.visible = visible && insideOutput;
    next.style = next.visible ? style : PointerCursorStyle::kDefault;
    if (next.visible) {
        next.x = std::clamp(static_cast<double>(relativeX - offsetX) /
                                std::max(1, outputWidth - 1),
                            0.0, 1.0);
        next.y = std::clamp(static_cast<double>(relativeY - offsetY) /
                                std::max(1, outputHeight - 1),
                            0.0, 1.0);
    }

    constexpr double kPositionEpsilon = 0.00001;
    const bool changed = !pointerKnown_ || next.visible != pointer_.visible ||
        (next.visible && next.style != pointer_.style) ||
        (next.visible && (std::abs(next.x - pointer_.x) > kPositionEpsilon ||
                          std::abs(next.y - pointer_.y) > kPositionEpsilon));
    if (changed) {
        pointer_ = next;
        pointerKnown_ = true;
        pointerDirty_ = true;
    }
}

void Capture::UpdatePointerFromFrame(const DXGI_OUTDUPL_FRAME_INFO& frameInfo) {
    // 仅位置未变但光标切换到文本/等待态时，PointerShapeBufferSize 仍会变化。不能
    // 只看 LastMouseUpdateTime，否则网页会一直保留旧箭头。
    if (frameInfo.LastMouseUpdateTime.QuadPart == 0 &&
        frameInfo.PointerShapeBufferSize == 0 && pointerKnown_) {
        return;
    }
    CURSORINFO cursorInfo{};
    cursorInfo.cbSize = sizeof(cursorInfo);
    if (GetCursorInfo(&cursorInfo)) {
        UpdatePointerFromDesktop((cursorInfo.flags & CURSOR_SHOWING) != 0,
                                 cursorInfo.ptScreenPos.x, cursorInfo.ptScreenPos.y,
                                 CursorStyleFromHandle(cursorInfo.hCursor));
        return;
    }
    UpdatePointerFromDesktop(frameInfo.PointerPosition.Visible != FALSE,
                             frameInfo.PointerPosition.Position.x,
                             frameInfo.PointerPosition.Position.y,
                             pointerKnown_ ? pointer_.style : PointerCursorStyle::kDefault);
}

void Capture::UpdatePointerFromSystem() {
    CURSORINFO cursorInfo{};
    cursorInfo.cbSize = sizeof(cursorInfo);
    if (!GetCursorInfo(&cursorInfo)) {
        return;
    }
    UpdatePointerFromDesktop((cursorInfo.flags & CURSOR_SHOWING) != 0,
                             cursorInfo.ptScreenPos.x, cursorInfo.ptScreenPos.y,
                             CursorStyleFromHandle(cursorInfo.hCursor));
}

void Capture::RecordGdiCaptureFailure(DWORD error) {
    const auto now = std::chrono::steady_clock::now();
    ++gdiConsecutiveFailures_;
    if (lastGdiFailureLogAt_.time_since_epoch().count() == 0 ||
        now - lastGdiFailureLogAt_ >= std::chrono::seconds(10)) {
        log::Warn("GDI screen copy failed: " + std::to_string(error) +
                  " consecutive=" + std::to_string(gdiConsecutiveFailures_));
        lastGdiFailureLogAt_ = now;
    }
}

void Capture::RecordGdiCaptureRecovery() {
    if (gdiConsecutiveFailures_ == 0) {
        return;
    }
    log::Info("GDI screen copy recovered after " + std::to_string(gdiConsecutiveFailures_) +
              " consecutive failures");
    gdiConsecutiveFailures_ = 0;
    lastGdiFailureLogAt_ = {};
}

bool Capture::EnumMonitors() {
    std::lock_guard<std::mutex> lock(monitorMu_);
    const std::vector<MonitorInfo> previous = monitors_;
    const int previousSelection = selectedMonitor_.load();
    std::string selectedDevice;
    if (previousSelection >= 0 && previousSelection < static_cast<int>(previous.size())) {
        selectedDevice = previous[previousSelection].name;
    }
    monitors_.clear();
    if (!EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(this))) {
        // 枚举异常时保留上一份完整拓扑。若把暂时性 Win32 失败当作“所有显示器已
        // 移除”，CaptureLoop 会重建 DXGI/MFT 并下发空 monitor 列表，远端会出现
        // 一次无意义的黑屏和解码器重置。
        const DWORD error = GetLastError();
        monitors_ = previous;
        log::Warn("display enumeration failed: " + std::to_string(error));
        return false;
    }

    // EnumDisplayMonitors 不保证枚举顺序。外部协议以 index 选择显示器，且布局
    // 比较依赖 vector 次序；若顺序偶发交换会误判拓扑变化，导致每秒重建采集和
    // H.264 编码器。按稳定的设备名与几何信息归一化后重新编号。
    std::sort(monitors_.begin(), monitors_.end(),
              [](const MonitorInfo& left, const MonitorInfo& right) {
                  if (left.name != right.name) return left.name < right.name;
                  if (left.x != right.x) return left.x < right.x;
                  if (left.y != right.y) return left.y < right.y;
                  if (left.w != right.w) return left.w < right.w;
                  return left.h < right.h;
              });
    for (size_t index = 0; index < monitors_.size(); ++index) {
        monitors_[index].index = static_cast<int>(index);
    }

    int nextSelection = -1;
    if (!selectedDevice.empty()) {
        for (const auto& monitor : monitors_) {
            if (monitor.name == selectedDevice) {
                nextSelection = monitor.index;
                break;
            }
        }
    } else if (previousSelection == -1) {
        nextSelection = -1;
    }
    selectedMonitor_.store(nextSelection);

    const bool layoutChanged = !SameMonitorLayout(previous, monitors_);
    const bool selectionChanged = previousSelection != nextSelection;
    if (layoutChanged || selectionChanged) {
        log::Info("display layout updated: " + std::to_string(monitors_.size()) +
                  " monitor(s), selected=" + std::to_string(nextSelection));
    }
    return layoutChanged || selectionChanged;
}

std::vector<MonitorInfo> Capture::MonitorsSnapshot() const {
    std::lock_guard<std::mutex> lock(monitorMu_);
    return monitors_;
}

void Capture::SetMonitor(int index) {
    std::lock_guard<std::mutex> lock(monitorMu_);
    selectedMonitor_.store(index >= -1 && index < static_cast<int>(monitors_.size()) ? index : -1);
}

void Capture::SetMaxOutputSize(int width, int height) {
    // NV12 要求偶数宽高；所有自适应档位都满足此条件，但在此处归一化以确保后续
    // CopyRegionToFrame 和 H.264 编码器的契约不被调用方破坏。
    width = std::max(2, width & ~1);
    height = std::max(2, height & ~1);
    if (maxOutputWidth_ == width && maxOutputHeight_ == height) {
        return;
    }
    maxOutputWidth_ = width;
    maxOutputHeight_ = height;
    // 下次缩放会按新的目标尺寸重建坐标表；立即清理无用表可避免长期切换档位时
    // 留下不再使用的大分配。
    scaleMapSourceWidth_ = 0;
    scaleMapSourceHeight_ = 0;
    scaleMapOutputWidth_ = 0;
    scaleMapOutputHeight_ = 0;
    scaleMapX_.clear();
    scaleMapY_.clear();
    log::Info("capture output cap set to " + std::to_string(maxOutputWidth_) + "x" +
              std::to_string(maxOutputHeight_));
}

void Capture::SetPatchCaptureEnabled(bool enabled) {
    if (patchCaptureEnabled_ == enabled) {
        return;
    }
    patchCaptureEnabled_ = enabled;
    // 切换局部模式不能沿用 GDI 上轮的像素基线；下一帧必须先作为完整画面发送。
    gdiPreviousFrame_.clear();
    log::Info(std::string("capture patch regions ") + (enabled ? "enabled" : "disabled"));
}

void Capture::SetPatchPrecision(PatchPrecision precision) {
    if (!IsPatchPrecisionValid(static_cast<int>(precision))) {
        precision = kLegacyPatchPrecision;
    }
    if (patchPrecision_ == precision) {
        return;
    }
    // 精度只影响下一帧的变化区域划分，不会改变 canvas 坐标、分辨率或已经确认
    // 的完整基线；因此不清空 GDI 基线，也不需要重建 DXGI/H.264 资源。
    patchPrecision_ = precision;
    log::Info(std::string("capture patch precision=") + PatchPrecisionName(precision));
}

void Capture::SetGpuOutputEnabled(bool enabled) {
    // GPU 直通只适用于 DXGI 单输出；视频处理器可同时完成保持比例的缩放。GDI
    // 回退时 CaptureFrame 会自然改走 CPU 帧，无需由调用者额外清理 surface。
    gpuOutputEnabled_ = enabled &&
        gpuOutputDisabledGeneration_ != deviceGeneration_;
    if (!enabled) {
        videoProcessor_.Reset();
        videoProcessorEnumerator_.Reset();
        videoProcessorSourceWidth_ = 0;
        videoProcessorSourceHeight_ = 0;
        videoProcessorOutputWidth_ = 0;
        videoProcessorOutputHeight_ = 0;
    }
}

void Capture::DisableGpuOutputForCurrentDevice() {
    // 仅关闭当前 DXGI device 的 GPU 通路。设备重建后 generation 会递增，新的
    // 显卡/输出仍可重新探测；同一设备上则始终走已经验证过的 CPU 回退路径。
    gpuOutputDisabledGeneration_ = deviceGeneration_;
    gpuOutputEnabled_ = false;
    videoProcessor_.Reset();
    videoProcessorEnumerator_.Reset();
    videoProcessorSourceWidth_ = 0;
    videoProcessorSourceHeight_ = 0;
    videoProcessorOutputWidth_ = 0;
    videoProcessorOutputHeight_ = 0;
}

bool Capture::Init() {
    if (InitDXGI()) {
        use_gdi_ = false;
        activeMonitor_ = selectedMonitor_.load();
        log::Info("capture: DXGI path ready");
        return true;
    }
    log::Warn("capture: DXGI init failed, fallback to GDI");
    use_gdi_ = true;
    activeMonitor_ = selectedMonitor_.load();
    return true;
}

void Capture::ResetForDesktop() {
    ReleaseAll();
    if (InitDXGI()) {
        use_gdi_ = false;
        activeMonitor_ = selectedMonitor_.load();
        log::Info("capture: DXGI path reinitialized after desktop change");
        return;
    }
    use_gdi_ = true;
    activeMonitor_ = selectedMonitor_.load();
    log::Warn("capture: DXGI reinit failed after desktop change, fallback to GDI");
}

bool Capture::InitDXGI() {
    const int selectedMonitor = selectedMonitor_.load();
    const std::vector<MonitorInfo> monitors = MonitorsSnapshot();
    if (selectedMonitor < -1 || selectedMonitor >= static_cast<int>(monitors.size())) {
        return false;
    }
    // “全部屏幕”在同一 adapter 的多输出机器上由 GPU 合成；跨 adapter、旋转屏
    // 或无法完整枚举的拓扑继续回退 GDI。选择单屏则直接绑定该输出。
    if (selectedMonitor == -1 && monitors.size() > 1) {
        return InitDXGIComposite(monitors);
    }

    std::wstring targetDevice;
    if (selectedMonitor >= 0) {
        const auto& name = monitors[selectedMonitor].name;
        targetDevice.assign(name.begin(), name.end());  // \\.\DISPLAYn 为 ASCII 设备名。
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
        return false;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapterResult = fac->EnumAdapters1(adapterIndex, &adapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapterResult)) {
            continue;
        }
        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> baseOutput;
            const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &baseOutput);
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(outputResult)) {
                continue;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(baseOutput->GetDesc(&desc)) ||
                (!targetDevice.empty() && _wcsicmp(desc.DeviceName, targetDevice.c_str()) != 0)) {
                continue;
            }

            Microsoft::WRL::ComPtr<IDXGIOutput1> output;
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
            D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
            if (FAILED(baseOutput.As(&output))) {
                continue;
            }
            HRESULT createDeviceResult = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
            HRESULT duplicateResult = FAILED(createDeviceResult) ? createDeviceResult :
                output->DuplicateOutput(device.Get(), &duplication);
            // 部分远程桌面/旧驱动不能创建 VIDEO_SUPPORT 设备，但仍可用 Desktop
            // Duplication。此时保留 CPU 编码路径，而不是把整个采集退化到 GDI。
            if (FAILED(createDeviceResult) || FAILED(duplicateResult)) {
                duplication.Reset();
                context.Reset();
                device.Reset();
                createDeviceResult = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                    &device, &featureLevel, &context);
                duplicateResult = FAILED(createDeviceResult) ? createDeviceResult :
                    output->DuplicateOutput(device.Get(), &duplication);
            }
            if (FAILED(createDeviceResult) || FAILED(duplicateResult)) {
                continue;
            }

            d3d_ = std::move(device);
            ctx_ = std::move(context);
            output_ = std::move(output);
            dup_ = std::move(duplication);
            ++deviceGeneration_;
            return true;
        }
    }
    return false;
}

bool Capture::InitDXGIComposite(const std::vector<MonitorInfo>& monitors) {
    if (monitors.size() < 2) {
        return false;
    }
    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 0 || virtualHeight <= 0) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
    std::vector<DxgiOutputCapture> selectedOutputs;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapterResult)) {
            continue;
        }

        std::vector<DxgiOutputCapture> outputs;
        bool unsupportedRotation = false;
        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> baseOutput;
            const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &baseOutput);
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(outputResult)) {
                continue;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(baseOutput->GetDesc(&desc))) {
                continue;
            }
            const auto monitor = std::find_if(monitors.begin(), monitors.end(),
                [&desc](const MonitorInfo& info) {
                    const std::wstring name(info.name.begin(), info.name.end());
                    return _wcsicmp(desc.DeviceName, name.c_str()) == 0;
                });
            if (monitor == monitors.end()) {
                continue;
            }
            // Desktop Duplication 在旋转输出上会返回未旋转的 surface；直接 copy
            // 到虚拟坐标会错位，保留 GDI 路径直至实现 GPU 旋转合成。
            if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
                unsupportedRotation = true;
                break;
            }
            Microsoft::WRL::ComPtr<IDXGIOutput1> output;
            if (FAILED(baseOutput.As(&output))) {
                unsupportedRotation = true;
                break;
            }
            DxgiOutputCapture state;
            state.output = std::move(output);
            state.x = monitor->x;
            state.y = monitor->y;
            state.width = monitor->w;
            state.height = monitor->h;
            outputs.push_back(std::move(state));
        }
        if (!unsupportedRotation && outputs.size() == monitors.size()) {
            selectedAdapter = std::move(adapter);
            selectedOutputs = std::move(outputs);
            break;
        }
    }
    if (!selectedAdapter.Get()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::vector<DxgiOutputCapture> outputs;
    auto createDeviceAndDuplications = [&](UINT flags) {
        device.Reset();
        context.Reset();
        outputs.clear();
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     flags, nullptr, 0, D3D11_SDK_VERSION, &device,
                                     &featureLevel, &context))) {
            return false;
        }
        outputs = selectedOutputs;
        for (auto& output : outputs) {
            if (FAILED(output.output->DuplicateOutput(device.Get(), &output.duplication))) {
                outputs.clear();
                context.Reset();
                device.Reset();
                return false;
            }
        }
        return true;
    };

    if (!createDeviceAndDuplications(D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT) &&
        !createDeviceAndDuplications(D3D11_CREATE_DEVICE_BGRA_SUPPORT)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC compositeDesc{};
    compositeDesc.Width = static_cast<UINT>(virtualWidth);
    compositeDesc.Height = static_cast<UINT>(virtualHeight);
    compositeDesc.MipLevels = 1;
    compositeDesc.ArraySize = 1;
    compositeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    compositeDesc.SampleDesc.Count = 1;
    compositeDesc.Usage = D3D11_USAGE_DEFAULT;
    compositeDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> composite;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> compositeRtv;
    if (FAILED(device->CreateTexture2D(&compositeDesc, nullptr, &composite)) ||
        FAILED(device->CreateRenderTargetView(composite.Get(), nullptr, &compositeRtv))) {
        return false;
    }

    d3d_ = std::move(device);
    ctx_ = std::move(context);
    dxgiOutputs_ = std::move(outputs);
    composite_ = std::move(composite);
    compositeRtv_ = std::move(compositeRtv);
    compositeWidth_ = virtualWidth;
    compositeHeight_ = virtualHeight;
    dxgiComposite_ = true;
    ++deviceGeneration_;
    log::Info("capture: DXGI multi-output composite ready for " +
              std::to_string(dxgiOutputs_.size()) + " monitor(s)");
    return true;
}

CaptureResult Capture::CaptureFrame(CapturedFrame& out, DWORD waitMs, bool requireFreshFrame) {
    // 采集线程串行使用同一个 staging texture。即使调用方因编码异常遗漏释放，也不
    // 能让下一次 Map 保持旧映射；正常路径会在编码后主动调用 ReleaseFrame。
    ReleaseFrame(out);
    out.cpuCopyOrScaleUs = 0;
    out.gdiBltUs = 0;
    const int selectedMonitor = selectedMonitor_.load();
    if (selectedMonitor != activeMonitor_) {
        // 切换单屏时重建到对应 adapter/output；“全部屏幕”会优先尝试同 adapter
        // 的 DXGI 合成，无法覆盖的拓扑再自动保留 GDI 回退。
        ResetForDesktop();
    }
    if (use_gdi_) {
        return CaptureGDI(out, requireFreshFrame);
    }

    const CaptureResult result = dxgiComposite_
        ? CaptureDXGIComposite(out, std::max<DWORD>(1, waitMs))
        : CaptureDXGI(out, std::max<DWORD>(1, waitMs));
    if (dxgiComposite_ && compositeNeedsGdiFrame_) {
        compositeNeedsGdiFrame_ = false;
        log::Info("capture: DXGI composite baseline incomplete, using one-shot GDI fallback");
        return CaptureGDI(out, true);
    }
    if (result != CaptureResult::kFailed) {
        // Desktop Duplication 在新订阅的静态桌面上可能只等到超时或 pointer-only
        // 通知。控制端尚未有可绘制画面时不能一直等下一次桌面变化，使用当前 input
        // desktop 的 GDI 快照建立一次完整基线；之后仍回到成本更低的 DXGI 路径。
        if (requireFreshFrame &&
            (result == CaptureResult::kNoChange || result == CaptureResult::kPointerOnly)) {
            log::Info("capture: DXGI has no full bootstrap frame, using one-shot GDI fallback");
            return CaptureGDI(out, true);
        }
        return result;
    }

    // DXGI 访问丢失后的资源重建交给 CaptureLoop 统一调度。这里若每一帧都立即
    // ResetForDesktop，显卡驱动异常或黑屏时会以目标帧率反复创建 D3D11/DXGI 对象，
    // 反而放大 CPU、GPU 与日志压力；调用方会先重建一次，再按指数退避重试。
    return CaptureResult::kFailed;
}

bool Capture::EnsureVideoProcessor(int sourceWidth, int sourceHeight,
                                   int outputWidth, int outputHeight) {
    if (!d3d_.Get() || !ctx_.Get()) {
        return false;
    }
    if (!videoDevice_.Get() && FAILED(d3d_.As(&videoDevice_))) {
        return false;
    }
    if (!videoContext_.Get() && FAILED(ctx_.As(&videoContext_))) {
        return false;
    }
    if (videoProcessor_.Get() &&
        videoProcessorSourceWidth_ == sourceWidth && videoProcessorSourceHeight_ == sourceHeight &&
        videoProcessorOutputWidth_ == outputWidth && videoProcessorOutputHeight_ == outputHeight) {
        return true;
    }

    videoProcessor_.Reset();
    videoProcessorEnumerator_.Reset();
    videoProcessorSourceWidth_ = 0;
    videoProcessorSourceHeight_ = 0;
    videoProcessorOutputWidth_ = 0;
    videoProcessorOutputHeight_ = 0;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = static_cast<UINT>(sourceWidth);
    content.InputHeight = static_cast<UINT>(sourceHeight);
    content.OutputWidth = static_cast<UINT>(outputWidth);
    content.OutputHeight = static_cast<UINT>(outputHeight);
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&content, &videoProcessorEnumerator_))) {
        return false;
    }

    UINT bgraSupport = 0;
    UINT nv12Support = 0;
    if (FAILED(videoProcessorEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &bgraSupport)) ||
        FAILED(videoProcessorEnumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12Support)) ||
        (bgraSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        (nv12Support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0 ||
        FAILED(videoDevice_->CreateVideoProcessor(videoProcessorEnumerator_.Get(), 0,
                                                   &videoProcessor_))) {
        videoProcessorEnumerator_.Reset();
        return false;
    }

    videoProcessorSourceWidth_ = sourceWidth;
    videoProcessorSourceHeight_ = sourceHeight;
    videoProcessorOutputWidth_ = outputWidth;
    videoProcessorOutputHeight_ = outputHeight;
    return true;
}

bool Capture::CreateGpuNv12Frame(ID3D11Texture2D* source, int width, int height,
                                 CapturedFrame& out) {
    if (!gpuOutputEnabled_ || !source || width <= 0 || height <= 0) {
        return false;
    }
    const auto preparationStartedAt = std::chrono::steady_clock::now();
    const double scale = std::min(1.0, std::min(
        static_cast<double>(maxOutputWidth_) / width,
        static_cast<double>(maxOutputHeight_) / height));
    int outputWidth = std::max(1, static_cast<int>(width * scale));
    int outputHeight = std::max(1, static_cast<int>(height * scale));
    if (outputWidth > 1) outputWidth &= ~1;
    if (outputHeight > 1) outputHeight &= ~1;
    if (outputWidth < 2 || outputHeight < 2 || (width & 1) != 0 || (height & 1) != 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || sourceDesc.SampleDesc.Count != 1 ||
        !EnsureVideoProcessor(width, height, outputWidth, outputHeight)) {
        DisableGpuOutputForCurrentDevice();
        log::Info("capture: GPU NV12 conversion unavailable, falling back to CPU frames");
        return false;
    }

    // MFT 可以异步持有输入 sample；若复用上一帧的 NV12 texture，下一次
    // VideoProcessorBlt 会覆盖仍在编码的内容。每帧拥有独立 texture，纹理会随
    // IMFSample 的最后一个引用释放，从根源上避免黑屏、花屏和驱动竞态。
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(outputWidth);
    textureDesc.Height = static_cast<UINT>(outputHeight);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_NV12;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Texture;
    if (FAILED(d3d_->CreateTexture2D(&textureDesc, nullptr, &nv12Texture))) {
        DisableGpuOutputForCurrentDevice();
        log::Info("capture: GPU NV12 texture allocation failed, falling back to CPU frames");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    if (FAILED(videoDevice_->CreateVideoProcessorInputView(source,
            videoProcessorEnumerator_.Get(), &inputDesc, &inputView))) {
        DisableGpuOutputForCurrentDevice();
        log::Info("capture: GPU input view unavailable, falling back to CPU frames");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(videoDevice_->CreateVideoProcessorOutputView(nv12Texture.Get(),
            videoProcessorEnumerator_.Get(), &outputDesc, &outputView))) {
        DisableGpuOutputForCurrentDevice();
        log::Info("capture: GPU output view unavailable, falling back to CPU frames");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    if (FAILED(videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView.Get(),
                                                0, 1, &stream))) {
        DisableGpuOutputForCurrentDevice();
        log::Info("capture: GPU BGRA to NV12 conversion failed, falling back to CPU frames");
        return false;
    }
    out.width = outputWidth;
    out.height = outputHeight;
    out.data.clear();
    out.mappedPixels = nullptr;
    out.mappedStrideBytes = 0;
    out.nv12Texture = std::move(nv12Texture);
    out.gpuNv12SubmissionUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - preparationStartedAt).count());
    return true;
}

std::vector<DirtyRegion> Capture::ReadDxgiDirtyRegions(
    IDXGIOutputDuplication* duplication, const DXGI_OUTDUPL_FRAME_INFO& frameInfo,
    int offsetX, int offsetY) {
    std::vector<DirtyRegion> regions;
    if (!duplication) {
        return regions;
    }
    const auto appendRect = [&regions, offsetX, offsetY](const RECT& rect) {
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width > 0 && height > 0) {
            regions.push_back({rect.left + offsetX, rect.top + offsetY, width, height});
        }
    };
    const auto readDirty = [&](UINT bytes) {
        if (bytes == 0) {
            return;
        }
        std::vector<RECT> rects(bytes / sizeof(RECT));
        UINT written = 0;
        if (SUCCEEDED(duplication->GetFrameDirtyRects(bytes, rects.data(), &written))) {
            const size_t count = std::min(rects.size(), static_cast<size_t>(written / sizeof(RECT)));
            for (size_t index = 0; index < count; ++index) {
                appendRect(rects[index]);
            }
        }
    };
    const auto readMoves = [&](UINT bytes) {
        if (bytes == 0) {
            return;
        }
        std::vector<DXGI_OUTDUPL_MOVE_RECT> moves(bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT));
        UINT written = 0;
        if (SUCCEEDED(duplication->GetFrameMoveRects(bytes, moves.data(), &written))) {
            const size_t count = std::min(moves.size(),
                static_cast<size_t>(written / sizeof(DXGI_OUTDUPL_MOVE_RECT)));
            for (size_t index = 0; index < count; ++index) {
                const auto& move = moves[index];
                appendRect(move.DestinationRect);
                // 浏览器不实现 DXGI 的“复制旧矩形”操作，保守地把源区域也作为
                // 变化发送，确保移动后露出的像素不会残留在客户端画布上。
                const int width = move.DestinationRect.right - move.DestinationRect.left;
                const int height = move.DestinationRect.bottom - move.DestinationRect.top;
                const RECT source = {move.SourcePoint.x, move.SourcePoint.y,
                                     move.SourcePoint.x + width, move.SourcePoint.y + height};
                appendRect(source);
            }
        }
    };

    UINT dirtyBytes = 0;
    const HRESULT dirtySizeResult = duplication->GetFrameDirtyRects(0, nullptr, &dirtyBytes);
    if (SUCCEEDED(dirtySizeResult) || dirtySizeResult == DXGI_ERROR_MORE_DATA) {
        readDirty(dirtyBytes);
    }
    UINT moveBytes = 0;
    const HRESULT moveSizeResult = duplication->GetFrameMoveRects(0, nullptr, &moveBytes);
    if (SUCCEEDED(moveSizeResult) || moveSizeResult == DXGI_ERROR_MORE_DATA) {
        readMoves(moveBytes);
    }
    (void)frameInfo;
    return regions;
}

void Capture::SetOutputDirtyRegions(const std::vector<DirtyRegion>& sourceRegions,
                                    int sourceWidth, int sourceHeight, CapturedFrame& out) {
    out.hasDirtyRegions = true;
    out.dirtyRegions.clear();
    if (sourceWidth <= 0 || sourceHeight <= 0 || out.width <= 0 || out.height <= 0) {
        return;
    }
    for (const DirtyRegion& source : sourceRegions) {
        const int sourceLeft = std::clamp(source.x, 0, sourceWidth);
        const int sourceTop = std::clamp(source.y, 0, sourceHeight);
        const int sourceRight = std::clamp(source.x + source.width, 0, sourceWidth);
        const int sourceBottom = std::clamp(source.y + source.height, 0, sourceHeight);
        if (sourceRight <= sourceLeft || sourceBottom <= sourceTop) {
            continue;
        }
        const int left = std::clamp(static_cast<int>(
            static_cast<int64_t>(sourceLeft) * out.width / sourceWidth), 0, out.width);
        const int top = std::clamp(static_cast<int>(
            static_cast<int64_t>(sourceTop) * out.height / sourceHeight), 0, out.height);
        const int right = std::clamp(static_cast<int>(
            (static_cast<int64_t>(sourceRight) * out.width + sourceWidth - 1) / sourceWidth),
            0, out.width);
        const int bottom = std::clamp(static_cast<int>(
            (static_cast<int64_t>(sourceBottom) * out.height + sourceHeight - 1) / sourceHeight),
            0, out.height);
        if (right > left && bottom > top) {
            out.dirtyRegions.push_back({left, top, right - left, bottom - top});
        }
    }
}

std::vector<DirtyRegion> Capture::DiffGdiTiles(const uint8_t* pixels, int width, int height,
                                                int leafTileSize) {
    std::vector<DirtyRegion> regions;
    if (!pixels || width <= 0 || height <= 0) {
        return regions;
    }
    const size_t stride = static_cast<size_t>(width) * 4;
    const size_t bytes = stride * static_cast<size_t>(height);
    if (gdiPreviousFrame_.size() != bytes) {
        gdiPreviousFrame_.assign(pixels, pixels + bytes);
        regions.push_back({0, 0, width, height});
        return regions;
    }
    if (leafTileSize <= 0 || leafTileSize > kPatchCoarseTileSize ||
        kPatchCoarseTileSize % leafTileSize != 0) {
        leafTileSize = kPatchCoarseTileSize;
    }
    const auto regionChanged = [&](int x, int y, int regionWidth, int regionHeight) {
        for (int row = 0; row < regionHeight; ++row) {
            const size_t offset = static_cast<size_t>(y + row) * stride +
                static_cast<size_t>(x) * 4;
            if (std::memcmp(pixels + offset, gdiPreviousFrame_.data() + offset,
                            static_cast<size_t>(regionWidth) * 4) != 0) {
                return true;
            }
        }
        return false;
    };

    // GDI 没有脏矩形元数据。先保留旧版 64x64 的粗筛成本；只有粗块发生变化时，
    // 才进一步检查 32x32 或 16x16 子块。静止桌面不会增加比较次数，两处相距较远
    // 的细小变化也不会因同属不同粗块被扩展成中间的大区域。
    for (int coarseY = 0; coarseY < height; coarseY += kPatchCoarseTileSize) {
        const int coarseHeight = std::min(kPatchCoarseTileSize, height - coarseY);
        for (int coarseX = 0; coarseX < width; coarseX += kPatchCoarseTileSize) {
            const int coarseWidth = std::min(kPatchCoarseTileSize, width - coarseX);
            if (!regionChanged(coarseX, coarseY, coarseWidth, coarseHeight)) {
                continue;
            }
            if (leafTileSize == kPatchCoarseTileSize) {
                regions.push_back({coarseX, coarseY, coarseWidth, coarseHeight});
                continue;
            }
            for (int y = coarseY; y < coarseY + coarseHeight; y += leafTileSize) {
                const int tileHeight = std::min(leafTileSize, coarseY + coarseHeight - y);
                for (int x = coarseX; x < coarseX + coarseWidth; x += leafTileSize) {
                    const int tileWidth = std::min(leafTileSize, coarseX + coarseWidth - x);
                    if (regionChanged(x, y, tileWidth, tileHeight)) {
                        regions.push_back({x, y, tileWidth, tileHeight});
                    }
                }
            }
        }
    }
    std::memcpy(gdiPreviousFrame_.data(), pixels, bytes);
    return regions;
}

CaptureResult Capture::CaptureDXGIComposite(CapturedFrame& out, DWORD waitMs) {
    if (!dxgiComposite_ || !composite_.Get() || !compositeRtv_.Get() ||
        dxgiOutputs_.empty() || !ctx_.Get() || !d3d_.Get()) {
        return CaptureResult::kFailed;
    }

    bool desktopChanged = false;
    std::vector<DirtyRegion> dirtyRegions;
    for (size_t index = 0; index < dxgiOutputs_.size(); ++index) {
        auto& output = dxgiOutputs_[index];
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        // 第一块输出按帧周期等待，剩余输出零等待轮询。若每个输出都等待一个
        // 周期，多屏会把 30 FPS 的采集延迟放大到显示器数量倍。
        const DWORD outputWaitMs = index == 0 ? waitMs : 0;
        const HRESULT hr = output.duplication->AcquireNextFrame(outputWaitMs, &frameInfo,
                                                                 &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (FAILED(hr)) {
            log::Warn("DXGI composite AcquireNextFrame failed hr=" + std::to_string(hr));
            return CaptureResult::kFailed;
        }

        UpdatePointerFromFrame(frameInfo);
        const bool pointerOnly = frameInfo.LastPresentTime.QuadPart == 0 &&
            frameInfo.AccumulatedFrames == 0 && frameInfo.TotalMetadataBufferSize == 0;
        if (pointerOnly) {
            output.duplication->ReleaseFrame();
            continue;
        }

        const std::vector<DirtyRegion> outputDirty = ReadDxgiDirtyRegions(
            output.duplication.Get(), frameInfo, output.x, output.y);
        dirtyRegions.insert(dirtyRegions.end(), outputDirty.begin(), outputDirty.end());

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) {
            output.duplication->ReleaseFrame();
            return CaptureResult::kFailed;
        }
        D3D11_TEXTURE2D_DESC sourceDesc{};
        texture->GetDesc(&sourceDesc);
        if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            sourceDesc.SampleDesc.Count != 1 ||
            static_cast<int>(sourceDesc.Width) != output.width ||
            static_cast<int>(sourceDesc.Height) != output.height) {
            output.duplication->ReleaseFrame();
            log::Warn("DXGI composite output size or format changed unexpectedly");
            return CaptureResult::kFailed;
        }

        D3D11_TEXTURE2D_DESC cachedDesc{};
        if (output.latestFrame.Get()) {
            output.latestFrame->GetDesc(&cachedDesc);
        }
        if (!output.latestFrame.Get() || cachedDesc.Width != sourceDesc.Width ||
            cachedDesc.Height != sourceDesc.Height || cachedDesc.Format != sourceDesc.Format) {
            output.latestFrame.Reset();
            D3D11_TEXTURE2D_DESC copyDesc = sourceDesc;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.BindFlags = 0;
            copyDesc.CPUAccessFlags = 0;
            copyDesc.MiscFlags = 0;
            if (FAILED(d3d_->CreateTexture2D(&copyDesc, nullptr, &output.latestFrame))) {
                output.duplication->ReleaseFrame();
                return CaptureResult::kFailed;
            }
        }
        ctx_->CopyResource(output.latestFrame.Get(), texture.Get());
        output.duplication->ReleaseFrame();
        output.hasFrame = true;
        desktopChanged = true;
    }

    if (!desktopChanged) {
        // 复用单屏路径的首帧行为：静态桌面且尚未有可用画面时由 CaptureFrame
        // 触发一次 GDI bootstrap；仅光标变化则不必重新编码整张合成帧。
        if (!pointerKnown_) {
            UpdatePointerFromSystem();
        }
        return pointerDirty_ ? CaptureResult::kPointerOnly : CaptureResult::kNoChange;
    }
    for (const auto& output : dxgiOutputs_) {
        if (!output.hasFrame || !output.latestFrame.Get()) {
            compositeNeedsGdiFrame_ = true;
            return CaptureResult::kNoChange;
        }
    }

    constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx_->ClearRenderTargetView(compositeRtv_.Get(), kBlack);
    for (const auto& output : dxgiOutputs_) {
        ctx_->CopySubresourceRegion(composite_.Get(), 0, static_cast<UINT>(output.x),
                                    static_cast<UINT>(output.y), 0,
                                    output.latestFrame.Get(), 0, nullptr);
    }

    if (!patchCaptureEnabled_ &&
        CreateGpuNv12Frame(composite_.Get(), compositeWidth_, compositeHeight_, out)) {
        width_ = out.width;
        height_ = out.height;
        SetOutputDirtyRegions(dirtyRegions, compositeWidth_, compositeHeight_, out);
        return CaptureResult::kFrame;
    }

    D3D11_TEXTURE2D_DESC compositeDesc{};
    composite_->GetDesc(&compositeDesc);
    D3D11_TEXTURE2D_DESC stagingDesc{};
    if (staging_.Get()) {
        staging_->GetDesc(&stagingDesc);
    }
    if (!staging_.Get() || stagingDesc.Width != compositeDesc.Width ||
        stagingDesc.Height != compositeDesc.Height || stagingDesc.Format != compositeDesc.Format) {
        staging_.Reset();
        D3D11_TEXTURE2D_DESC readbackDesc = compositeDesc;
        readbackDesc.Usage = D3D11_USAGE_STAGING;
        readbackDesc.BindFlags = 0;
        readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        readbackDesc.MiscFlags = 0;
        if (FAILED(d3d_->CreateTexture2D(&readbackDesc, nullptr, &staging_))) {
            return CaptureResult::kFailed;
        }
    }
    ctx_->CopyResource(staging_.Get(), composite_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return CaptureResult::kFailed;
    }

    const bool canDirectMap = compositeWidth_ <= maxOutputWidth_ &&
        compositeHeight_ <= maxOutputHeight_ && (compositeWidth_ & 1) == 0 &&
        (compositeHeight_ & 1) == 0;
    if (canDirectMap) {
        out.width = compositeWidth_;
        out.height = compositeHeight_;
        out.data.clear();
        out.mappedPixels = static_cast<const uint8_t*>(mapped.pData);
        out.mappedStrideBytes = mapped.RowPitch;
        stagingMapped_ = true;
    } else {
        CopyRegionToFrame(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, 0, 0,
                          compositeWidth_, compositeHeight_, out);
        ctx_->Unmap(staging_.Get(), 0);
    }
    width_ = out.width;
    height_ = out.height;
    SetOutputDirtyRegions(dirtyRegions, compositeWidth_, compositeHeight_, out);
    return CaptureResult::kFrame;
}

CaptureResult Capture::CaptureDXGI(CapturedFrame& out, DWORD waitMs) {
    if (!dup_) return CaptureResult::kFailed;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    Microsoft::WRL::ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(waitMs, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // 刚连接到静态桌面时未必立刻获得首个 duplication frame；仍从当前 input
        // desktop 读取一次指针，避免浏览器长时间没有任何远端鼠标反馈。
        if (!pointerKnown_) {
            UpdatePointerFromSystem();
        }
        return CaptureResult::kNoChange;
    }
    if (FAILED(hr)) {
        log::Warn("DXGI AcquireNextFrame failed hr=" + std::to_string(hr));
        return CaptureResult::kFailed;
    }

    UpdatePointerFromFrame(fi);

    // Microsoft 文档保证：仅鼠标指针更新时，AccumulatedFrames、
    // TotalMetadataBufferSize 与 LastPresentTime 均为零。指针位置已通过独立状态
    // 更新，本次资源仍是上一张桌面图像，重编码它只会占用 CPU/GPU 和网络窗口。
    const bool pointerOnly = fi.LastPresentTime.QuadPart == 0 &&
        fi.AccumulatedFrames == 0 && fi.TotalMetadataBufferSize == 0;
    if (pointerOnly) {
        dup_->ReleaseFrame();
        return CaptureResult::kPointerOnly;
    }

    const std::vector<DirtyRegion> dirtyRegions =
        ReadDxgiDirtyRegions(dup_.Get(), fi);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) { dup_->ReleaseFrame(); return CaptureResult::kFailed; }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    // GPU path 在释放 Desktop Duplication 的当前帧前完成颜色转换，并把自有 NV12
    // surface 交给编码器。它避开了 staging Map、整帧 BGRA 回读和 CPU 色彩转换。
    if (!patchCaptureEnabled_ && CreateGpuNv12Frame(tex.Get(), w, h, out)) {
        dup_->ReleaseFrame();
        width_ = out.width;
        height_ = out.height;
        SetOutputDirtyRegions(dirtyRegions, w, h, out);
        return CaptureResult::kFrame;
    }
    D3D11_TEXTURE2D_DESC stagingDesc{};
    if (staging_) {
        staging_->GetDesc(&stagingDesc);
    }
    if (!staging_ || stagingDesc.Width != desc.Width || stagingDesc.Height != desc.Height ||
            stagingDesc.Format != desc.Format) {
        staging_.Reset();
        D3D11_TEXTURE2D_DESC s = desc;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        if (FAILED(d3d_->CreateTexture2D(&s, nullptr, &staging_))) {
            dup_->ReleaseFrame();
            return CaptureResult::kFailed;
        }
    }
    ctx_->CopyResource(staging_.Get(), tex.Get());
    dup_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return CaptureResult::kFailed;
    }
    // 未缩放且 NV12 兼容的偶数尺寸可直接借用 staging 映射。原实现会先完整复制
    // 到 CapturedFrame::data，再由编码器读取一次；此路径每帧省去约一张 BGRA 图像
    // 的 CPU 内存带宽（1080p 约 8 MB）。
    const bool canDirectMap = w <= maxOutputWidth_ && h <= maxOutputHeight_ &&
        (w & 1) == 0 && (h & 1) == 0;
    if (canDirectMap) {
        out.width = w;
        out.height = h;
        out.data.clear();
        out.mappedPixels = static_cast<const uint8_t*>(mapped.pData);
        out.mappedStrideBytes = mapped.RowPitch;
        stagingMapped_ = true;
        width_ = out.width;
        height_ = out.height;
        SetOutputDirtyRegions(dirtyRegions, w, h, out);
        return CaptureResult::kFrame;
    }

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    CopyRegionToFrame(src, mapped.RowPitch, 0, 0, w, h, out);
    ctx_->Unmap(staging_.Get(), 0);
    width_ = out.width;
    height_ = out.height;
    SetOutputDirtyRegions(dirtyRegions, w, h, out);
    return CaptureResult::kFrame;
}

CaptureResult Capture::CaptureGDI(CapturedFrame& out, bool forceFrame) {
    // 虚拟屏幕尺寸(涵盖所有显示器)。
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return CaptureResult::kFailed;

    // 确定源区域：全部虚拟屏幕或指定显示器。显示器热插拔可能让上一轮快照的
    // 坐标在本轮短暂过期，因此先裁剪；无交集时安全退回完整虚拟桌面。
    int ox = 0;
    int oy = 0;
    int ow = vw;
    int oh = vh;
    const int selectedMonitor = selectedMonitor_.load();
    const std::vector<MonitorInfo> monitors = MonitorsSnapshot();
    if (selectedMonitor >= 0 && selectedMonitor < static_cast<int>(monitors.size())) {
        const auto& monitor = monitors[selectedMonitor];
        const long long left = std::max<long long>(0, monitor.x);
        const long long top = std::max<long long>(0, monitor.y);
        const long long right = std::min<long long>(
            vw, static_cast<long long>(monitor.x) + monitor.w);
        const long long bottom = std::min<long long>(
            vh, static_cast<long long>(monitor.y) + monitor.h);
        if (right > left && bottom > top) {
            ox = static_cast<int>(left);
            oy = static_cast<int>(top);
            ow = static_cast<int>(right - left);
            oh = static_cast<int>(bottom - top);
        }
    }

    // 与 DXGI CPU 路径使用相同的等比例输出规则。GDI 直接写最终尺寸的 DIB，
    // 不再先 BitBlt 整个虚拟桌面、再 CopyRegionToFrame 做 CPU 裁剪/最近邻缩放。
    const double scale = std::min(1.0, std::min(
        static_cast<double>(maxOutputWidth_) / ow,
        static_cast<double>(maxOutputHeight_) / oh));
    int outputWidth = std::max(1, static_cast<int>(ow * scale));
    int outputHeight = std::max(1, static_cast<int>(oh * scale));
    if (outputWidth > 1) outputWidth &= ~1;
    if (outputHeight > 1) outputHeight &= ~1;
    if (outputWidth <= 0 || outputHeight <= 0) {
        return CaptureResult::kFailed;
    }

    // 优先取当前 input desktop 的 DC，锁屏/Winlogon 情况不会误用普通 DISPLAY DC。
    if (!gdi_dc_) {
        gdi_dc_ = GetDC(nullptr);
        gdi_dc_from_getdc_ = (gdi_dc_ != nullptr);
        if (!gdi_dc_) {
            gdi_dc_ = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        }
        if (!gdi_dc_) return CaptureResult::kFailed;
    }
    if (!mem_dc_) {
        mem_dc_ = CreateCompatibleDC(gdi_dc_);
        if (!mem_dc_) return CaptureResult::kFailed;
    }
    if (!bmp_ || dibW_ != outputWidth || dibH_ != outputHeight) {
        if (bmp_) {
            SelectObject(mem_dc_, old_bmp_);
            DeleteObject(bmp_);
            bmp_ = nullptr;
            bits_ = nullptr;
            dibW_ = 0;
            dibH_ = 0;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = outputWidth;
        bi.bmiHeader.biHeight = -outputHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bmp_) return CaptureResult::kFailed;
        const HGDIOBJ oldBitmap = SelectObject(mem_dc_, bmp_);
        if (!oldBitmap || oldBitmap == HGDI_ERROR) {
            DeleteObject(bmp_);
            bmp_ = nullptr;
            bits_ = nullptr;
            return CaptureResult::kFailed;
        }
        old_bmp_ = static_cast<HBITMAP>(oldBitmap);
        dibW_ = outputWidth;
        dibH_ = outputHeight;
        // DIB 尺寸改变后首帧必须进入编码器，不能复用旧尺寸的采样摘要。
        hasGdiFingerprint_.fill(false);
        gdiPreviousFrame_.clear();
    }
    if (!bits_) {
        return CaptureResult::kFailed;
    }

    // 使用虚拟桌面原点，避免有负坐标显示器时截取到错误区域。
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const bool sameSize = outputWidth == ow && outputHeight == oh;
    if (!sameSize) {
        // COLORONCOLOR 是低延迟的近邻伸缩；远控更应优先降低复制量和首帧时间，
        // 不使用会额外增加 CPU 的 HALFTONE 高质量缩放。
        SetStretchBltMode(mem_dc_, COLORONCOLOR);
    }
    const auto gdiBltStartedAt = std::chrono::steady_clock::now();
    const BOOL copied = sameSize
        ? BitBlt(mem_dc_, 0, 0, outputWidth, outputHeight, gdi_dc_, vx + ox, vy + oy,
                 SRCCOPY | CAPTUREBLT)
        : StretchBlt(mem_dc_, 0, 0, outputWidth, outputHeight, gdi_dc_, vx + ox, vy + oy,
                     ow, oh, SRCCOPY | CAPTUREBLT);
    out.gdiBltUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - gdiBltStartedAt).count());
    if (!copied) {
        RecordGdiCaptureFailure(GetLastError());
        return CaptureResult::kFailed;
    }
    RecordGdiCaptureRecovery();
    // GDI 不提供 Desktop Duplication 的 pointer metadata；读取当前输入桌面的系统
    // 指针即可让锁屏和多屏回退路径也同步浏览器原生光标的可见性与样式。
    UpdatePointerFromSystem();

    const auto* src = static_cast<const uint8_t*>(bits_);
    const auto now = std::chrono::steady_clock::now();
    if (patchCaptureEnabled_) {
        out.hasDirtyRegions = true;
        out.dirtyRegions = DiffGdiTiles(src, outputWidth, outputHeight,
                                        PatchTileSize(patchPrecision_));
        if (!forceFrame && out.dirtyRegions.empty()) {
            return CaptureResult::kNoChange;
        }
        // 强制首帧/恢复时即使像素比较为空也必须下发完整画面；交给 Agent 根据
        // firstFrame 标志选择完整编码，因此这里标记整个输出为变化区域。
        if (forceFrame && out.dirtyRegions.empty()) {
            out.dirtyRegions.push_back({0, 0, outputWidth, outputHeight});
        }
    } else {
        constexpr size_t kSampleStrideBytes = 64;
        bool fingerprintChanged = false;
        for (size_t phase = 0; phase < kGdiFingerprintPhaseCount; ++phase) {
            const uint64_t fingerprint = FingerprintBgraRegion(
                src, static_cast<size_t>(outputWidth) * 4, 0, 0, outputWidth, outputHeight,
                phase * (kSampleStrideBytes / kGdiFingerprintPhaseCount));
            fingerprintChanged = fingerprintChanged || !hasGdiFingerprint_[phase] ||
                fingerprint != gdiFingerprints_[phase];
            // 无论本次是否推帧，都更新所有相位的基线。这样真实变化只会触发一张
            // 编码帧，后续采样不会拿着变化前的其他相位摘要再次误判为新变化。
            gdiFingerprints_[phase] = fingerprint;
            hasGdiFingerprint_[phase] = true;
        }
        const bool refreshDue = lastGdiFullFrameAt_.time_since_epoch().count() == 0 ||
            now - lastGdiFullFrameAt_ >= std::chrono::seconds(1);
        if (!forceFrame && !fingerprintChanged && !refreshDue) {
            return CaptureResult::kNoChange;
        }
    }

    // DIB 已是最终尺寸，只需一次顺序复制给编码器。相比旧路径省去完整虚拟桌面
    // DIB、CPU 裁剪和逐像素缩放，锁屏/跨显卡多屏的内存带宽显著下降。
    out.width = outputWidth;
    out.height = outputHeight;
    // GDI 的最终 DIB 只由本采集线程读写：本轮编码会在下一次 BitBlt 前同步完成，
    // 因此可直接借用 DIB 给 H.264 的 BGRA->NV12 或 WIC JPEG 输入，省去每帧整张
    // BGRA 副本。CaptureFrame 下一轮开始和异常路径均会统一 ReleaseFrame 清除借用。
    out.data.clear();
    out.mappedPixels = src;
    out.mappedStrideBytes = static_cast<size_t>(outputWidth) * 4;
    out.borrowedGdiPixels = true;
    out.nv12Texture.Reset();
    lastGdiFullFrameAt_ = now;
    width_ = out.width;
    height_ = out.height;
    return CaptureResult::kFrame;
}

void Capture::CopyRegionToFrame(const uint8_t* source, size_t sourceStrideBytes,
                                int x, int y, int width, int height,
                                CapturedFrame& out) {
    const auto copyStartedAt = std::chrono::steady_clock::now();
    const double scale = std::min(1.0, std::min(
        static_cast<double>(maxOutputWidth_) / width,
        static_cast<double>(maxOutputHeight_) / height));
    // NV12/H.264 需要偶数宽高。统一在采集输出处向下取偶数，JPEG 回退也使用
    // 相同尺寸，避免浏览器在编码器切换时发生坐标和画布尺寸跳变。
    int outputWidth = std::max(1, static_cast<int>(width * scale));
    int outputHeight = std::max(1, static_cast<int>(height * scale));
    if (outputWidth > 1) outputWidth &= ~1;
    if (outputHeight > 1) outputHeight &= ~1;

    out.width = outputWidth;
    out.height = outputHeight;
    out.mappedPixels = nullptr;
    out.mappedStrideBytes = 0;
    out.borrowedGdiPixels = false;
    out.data.resize(static_cast<size_t>(outputWidth) * outputHeight * 4);

    if (outputWidth == width && outputHeight == height) {
        for (int row = 0; row < height; ++row) {
            const uint8_t* sourceRow = source + static_cast<size_t>(y + row) * sourceStrideBytes +
                static_cast<size_t>(x) * 4;
            std::memcpy(out.data.data() + static_cast<size_t>(row) * width * 4,
                        sourceRow, static_cast<size_t>(width) * 4);
        }
        out.cpuCopyOrScaleUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - copyStartedAt).count());
        return;
    }

    // 最近邻缩放避免额外依赖与大缓冲，优先保障远控的低延迟。源/目标尺寸不变时
    // 复用坐标表，避免在 4K -> 1080p 情况下每帧执行约两百万次除法。
    if (scaleMapSourceWidth_ != width || scaleMapSourceHeight_ != height ||
        scaleMapOutputWidth_ != outputWidth || scaleMapOutputHeight_ != outputHeight) {
        scaleMapSourceWidth_ = width;
        scaleMapSourceHeight_ = height;
        scaleMapOutputWidth_ = outputWidth;
        scaleMapOutputHeight_ = outputHeight;
        scaleMapX_.resize(static_cast<size_t>(outputWidth));
        scaleMapY_.resize(static_cast<size_t>(outputHeight));
        for (int outputX = 0; outputX < outputWidth; ++outputX) {
            scaleMapX_[outputX] = static_cast<size_t>(outputX) *
                static_cast<size_t>(width) / static_cast<size_t>(outputWidth) * 4;
        }
        for (int outputY = 0; outputY < outputHeight; ++outputY) {
            scaleMapY_[outputY] = outputY * height / outputHeight;
        }
    }
    const size_t regionXBytes = static_cast<size_t>(x) * 4;
    const bool integralHorizontalScale = width % outputWidth == 0;
    const size_t sourceStepBytes = integralHorizontalScale
        ? static_cast<size_t>(width / outputWidth) * 4
        : 0;
    for (int outputY = 0; outputY < outputHeight; ++outputY) {
        const int sourceY = y + scaleMapY_[outputY];
        uint8_t* destination = out.data.data() + static_cast<size_t>(outputY) * outputWidth * 4;
        const uint8_t* sourceRow = source + static_cast<size_t>(sourceY) * sourceStrideBytes +
            regionXBytes;
        uint8_t* destinationPixel = destination;
        if (integralHorizontalScale) {
            const uint8_t* pixel = sourceRow;
            for (int outputX = 0; outputX < outputWidth;
                 ++outputX, pixel += sourceStepBytes, destinationPixel += 4) {
                std::memcpy(destinationPixel, pixel, 4);
            }
        } else {
            for (int outputX = 0; outputX < outputWidth; ++outputX, destinationPixel += 4) {
                std::memcpy(destinationPixel, sourceRow + scaleMapX_[outputX], 4);
            }
        }
    }
    out.cpuCopyOrScaleUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - copyStartedAt).count());
}

bool Capture::MapNormalizedToVirtual(double x, double y, double& virtualX, double& virtualY) const {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);

    const int virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 0 || virtualHeight <= 0) {
        return false;
    }

    int offsetX = 0;
    int offsetY = 0;
    int width = virtualWidth;
    int height = virtualHeight;
    const int selectedMonitor = selectedMonitor_.load();
    const std::vector<MonitorInfo> monitors = MonitorsSnapshot();
    if (selectedMonitor >= 0 && selectedMonitor < static_cast<int>(monitors.size())) {
        const auto& monitor = monitors[selectedMonitor];
        offsetX = monitor.x;
        offsetY = monitor.y;
        width = monitor.w;
        height = monitor.h;
    }

    const int pixelX = offsetX + static_cast<int>(x * std::max(0, width - 1));
    const int pixelY = offsetY + static_cast<int>(y * std::max(0, height - 1));
    virtualX = static_cast<double>(pixelX) / std::max(1, virtualWidth - 1);
    virtualY = static_cast<double>(pixelY) / std::max(1, virtualHeight - 1);
    return true;
}

}  // namespace remote_assist
