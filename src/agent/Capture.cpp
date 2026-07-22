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

constexpr int kMaxStreamWidth = 1920;
constexpr int kMaxStreamHeight = 1080;

// 每行错开采样相位，避免固定列的细小变化一直漏检。采样比例约为 1/16，
// 相比完整 4K 帧的缩放/复制开销可忽略。
uint64_t FingerprintBgraRegion(const uint8_t* source, size_t sourceStrideBytes,
                               int x, int y, int width, int height) {
    constexpr size_t kSampleStrideBytes = 64;
    uint64_t hash = 1469598103934665603ULL;
    const size_t regionBytes = static_cast<size_t>(width) * 4;
    for (int row = 0; row < height; ++row) {
        const uint8_t* sourceRow = source + static_cast<size_t>(y + row) * sourceStrideBytes +
            static_cast<size_t>(x) * 4;
        const size_t phase = (static_cast<size_t>(row) * 20) % kSampleStrideBytes;
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
        // 通过 friend 或 public 接口添加 —— 用 const_cast 绕过 const(回调设计如此)。
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
    gdiFingerprint_ = 0;
    hasGdiFingerprint_ = false;
    lastGdiFullFrameAt_ = {};
}

void Capture::ReleaseFrame(CapturedFrame& frame) {
    if (stagingMapped_ && ctx_.Get() && staging_.Get()) {
        ctx_->Unmap(staging_.Get(), 0);
        stagingMapped_ = false;
    }
    frame.mappedPixels = nullptr;
    frame.mappedStrideBytes = 0;
}

void Capture::EnumMonitors() {
    monitors_.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(this));
    log::Info("enumerated " + std::to_string(monitors_.size()) + " monitor(s)");
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
    if (selectedMonitor < -1 || selectedMonitor >= static_cast<int>(monitors_.size())) {
        return false;
    }
    // “全部屏幕”在多适配器/多输出时仍需合成，保留 GDI 回退；但选择单屏时
    // 直接绑定该输出的 DXGI Desktop Duplication，不再因为机器有多屏而退化。
    if (selectedMonitor == -1 && monitors_.size() > 1) {
        return false;
    }

    std::wstring targetDevice;
    if (selectedMonitor >= 0) {
        const auto& name = monitors_[selectedMonitor].name;
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
            if (FAILED(baseOutput.As(&output)) ||
                FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                    &device, &featureLevel, &context)) ||
                FAILED(output->DuplicateOutput(device.Get(), &duplication))) {
                continue;
            }

            d3d_ = std::move(device);
            ctx_ = std::move(context);
            output_ = std::move(output);
            dup_ = std::move(duplication);
            return true;
        }
    }
    return false;
}

CaptureResult Capture::CaptureFrame(CapturedFrame& out, DWORD waitMs) {
    // 采集线程串行使用同一个 staging texture。即使调用方因编码异常遗漏释放，也不
    // 能让下一次 Map 保持旧映射；正常路径会在编码后主动调用 ReleaseFrame。
    ReleaseFrame(out);
    const int selectedMonitor = selectedMonitor_.load();
    if (selectedMonitor != activeMonitor_) {
        // 切换单屏时重建到对应 adapter/output；切回“全部”时保留 GDI 合成。
        ResetForDesktop();
    }
    const bool allMonitors = selectedMonitor == -1 && monitors_.size() > 1;
    if (use_gdi_ || allMonitors) {
        return CaptureGDI(out);
    }

    const CaptureResult result = CaptureDXGI(out, std::max<DWORD>(1, waitMs));
    if (result != CaptureResult::kFailed) {
        return result;
    }

    // DXGI 访问丢失等异常会在下一帧尝试重建，不把空闲超时误当异常。
    ResetForDesktop();
    return CaptureResult::kNoChange;
}

CaptureResult Capture::CaptureDXGI(CapturedFrame& out, DWORD waitMs) {
    if (!dup_) return CaptureResult::kFailed;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    Microsoft::WRL::ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(waitMs, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return CaptureResult::kNoChange;
    }
    if (FAILED(hr)) {
        log::Warn("DXGI AcquireNextFrame failed hr=" + std::to_string(hr));
        return CaptureResult::kFailed;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) { dup_->ReleaseFrame(); return CaptureResult::kFailed; }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
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
    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    // 未缩放且 NV12 兼容的偶数尺寸可直接借用 staging 映射。原实现会先完整复制
    // 到 CapturedFrame::data，再由编码器读取一次；此路径每帧省去约一张 BGRA 图像
    // 的 CPU 内存带宽（1080p 约 8 MB）。
    const bool canDirectMap = w <= kMaxStreamWidth && h <= kMaxStreamHeight &&
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
        return CaptureResult::kFrame;
    }

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    CopyRegionToFrame(src, mapped.RowPitch, 0, 0, w, h, out);
    ctx_->Unmap(staging_.Get(), 0);
    width_ = out.width;
    height_ = out.height;
    return CaptureResult::kFrame;
}

CaptureResult Capture::CaptureGDI(CapturedFrame& out) {
    // 虚拟屏幕尺寸(涵盖所有显示器)。
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return CaptureResult::kFailed;

    // 优先取当前 input desktop 的 DC，锁屏/Winlogon 情况不会误用普通 DISPLAY DC。
    // 创建/复用 DIB 的尺寸仍然覆盖整个虚拟屏幕。
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
    if (!bmp_ || dibW_ != vw || dibH_ != vh) {
        if (bmp_) { SelectObject(mem_dc_, old_bmp_); DeleteObject(bmp_); bmp_ = nullptr; }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = vw;
        bi.bmiHeader.biHeight = -vh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bmp_) return CaptureResult::kFailed;
        old_bmp_ = static_cast<HBITMAP>(SelectObject(mem_dc_, bmp_));
        dibW_ = vw; dibH_ = vh;
    }

    // 使用虚拟桌面原点，避免有负坐标显示器时截取到错误区域。
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (!BitBlt(mem_dc_, 0, 0, vw, vh, gdi_dc_, vx, vy, SRCCOPY | CAPTUREBLT)) {
        // GetDIBits 仅会读回当前 DIB 内容，不能作为屏幕采集失败后的替代方案。
        log::Warn("GDI BitBlt failed: " + std::to_string(GetLastError()));
        return CaptureResult::kFailed;
    }

    // 确定输出区域:全部虚拟屏幕或指定显示器。
    int ox = 0, oy = 0, ow = vw, oh = vh;
    const int selectedMonitor = selectedMonitor_.load();
    if (selectedMonitor >= 0 && selectedMonitor < static_cast<int>(monitors_.size())) {
        const auto& m = monitors_[selectedMonitor];
        ox = m.x; oy = m.y; ow = m.w; oh = m.h;
    }

    const auto* src = static_cast<const uint8_t*>(bits_);
    const auto now = std::chrono::steady_clock::now();
    const uint64_t fingerprint = FingerprintBgraRegion(src, static_cast<size_t>(vw) * 4,
                                                       ox, oy, ow, oh);
    const bool refreshDue = lastGdiFullFrameAt_.time_since_epoch().count() == 0 ||
        now - lastGdiFullFrameAt_ >= std::chrono::seconds(1);
    if (hasGdiFingerprint_ && fingerprint == gdiFingerprint_ && !refreshDue) {
        return CaptureResult::kNoChange;
    }
    gdiFingerprint_ = fingerprint;
    hasGdiFingerprint_ = true;

    // 原始 DIB 已确认有变化（或到达兜底刷新时间）后才裁剪、缩放并分配输出帧。
    CopyRegionToFrame(src, static_cast<size_t>(vw) * 4, ox, oy, ow, oh, out);
    lastGdiFullFrameAt_ = now;
    width_ = out.width;
    height_ = out.height;
    return CaptureResult::kFrame;
}

void Capture::CopyRegionToFrame(const uint8_t* source, size_t sourceStrideBytes,
                                int x, int y, int width, int height,
                                CapturedFrame& out) {
    const double scale = std::min(1.0, std::min(
        static_cast<double>(kMaxStreamWidth) / width,
        static_cast<double>(kMaxStreamHeight) / height));
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
    out.data.resize(static_cast<size_t>(outputWidth) * outputHeight * 4);

    if (outputWidth == width && outputHeight == height) {
        for (int row = 0; row < height; ++row) {
            const uint8_t* sourceRow = source + static_cast<size_t>(y + row) * sourceStrideBytes +
                static_cast<size_t>(x) * 4;
            std::memcpy(out.data.data() + static_cast<size_t>(row) * width * 4,
                        sourceRow, static_cast<size_t>(width) * 4);
        }
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
            scaleMapX_[outputX] = outputX * width / outputWidth;
        }
        for (int outputY = 0; outputY < outputHeight; ++outputY) {
            scaleMapY_[outputY] = outputY * height / outputHeight;
        }
    }
    for (int outputY = 0; outputY < outputHeight; ++outputY) {
        const int sourceY = y + scaleMapY_[outputY];
        uint8_t* destination = out.data.data() + static_cast<size_t>(outputY) * outputWidth * 4;
        for (int outputX = 0; outputX < outputWidth; ++outputX) {
            const int sourceX = x + scaleMapX_[outputX];
            const uint8_t* pixel = source + static_cast<size_t>(sourceY) * sourceStrideBytes +
                static_cast<size_t>(sourceX) * 4;
            std::memcpy(destination + static_cast<size_t>(outputX) * 4, pixel, 4);
        }
    }
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
    if (selectedMonitor >= 0 && selectedMonitor < static_cast<int>(monitors_.size())) {
        const auto& monitor = monitors_[selectedMonitor];
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
