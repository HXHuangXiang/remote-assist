#include "agent/Capture.h"

#include "common/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

namespace remote_assist {

namespace {

constexpr int kMaxStreamWidth = 1920;
constexpr int kMaxStreamHeight = 1080;

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
}

void Capture::EnumMonitors() {
    monitors_.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(this));
    log::Info("enumerated " + std::to_string(monitors_.size()) + " monitor(s)");
}

bool Capture::Init() {
    if (InitDXGI()) {
        use_gdi_ = false;
        log::Info("capture: DXGI path ready");
        return true;
    }
    log::Warn("capture: DXGI init failed, fallback to GDI");
    use_gdi_ = true;
    return true;
}

void Capture::ResetForDesktop() {
    ReleaseAll();
    if (InitDXGI()) {
        use_gdi_ = false;
        log::Info("capture: DXGI path reinitialized after desktop change");
        return;
    }
    use_gdi_ = true;
    log::Warn("capture: DXGI reinit failed after desktop change, fallback to GDI");
}

bool Capture::InitDXGI() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(fac->EnumAdapters1(0, &adapter))) return false;
    Microsoft::WRL::ComPtr<IDXGIOutput> out0;
    if (FAILED(adapter->EnumOutputs(0, &out0))) return false;
    if (FAILED(out0.As(&output_))) return false;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &d3d_, &fl, &ctx_))) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &d3d_, &fl, &ctx_))) return false;
    }
    if (FAILED(output_->DuplicateOutput(d3d_.Get(), &dup_))) return false;
    return true;
}

CaptureResult Capture::CaptureFrame(CapturedFrame& out) {
    // 当前 DXGI 实现只复制一个 output；选择单屏或存在多屏时走 GDI，
    // 保证“全部屏幕/指定屏幕”的图像与浏览器选择项一致。
    if (use_gdi_ || selectedMonitor_.load() >= 0 || monitors_.size() > 1) {
        return CaptureGDI(out) ? CaptureResult::kFrame : CaptureResult::kFailed;
    }

    const CaptureResult result = CaptureDXGI(out);
    if (result != CaptureResult::kFailed) {
        return result;
    }

    // DXGI 访问丢失等异常会在下一帧尝试重建，不把空闲超时误当异常。
    ResetForDesktop();
    return CaptureResult::kNoChange;
}

CaptureResult Capture::CaptureDXGI(CapturedFrame& out) {
    if (!dup_) return CaptureResult::kFailed;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    Microsoft::WRL::ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(50, &fi, &res);
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
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    CopyRegionToFrame(src, mapped.RowPitch, 0, 0, w, h, out);
    ctx_->Unmap(staging_.Get(), 0);
    width_ = out.width;
    height_ = out.height;
    return CaptureResult::kFrame;
}

bool Capture::CaptureGDI(CapturedFrame& out) {
    // 虚拟屏幕尺寸(涵盖所有显示器)。
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    // 创建/复用 DC 和 DIB(DIB 尺寸 = 虚拟屏幕)。
    if (!gdi_dc_) {
        gdi_dc_ = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        if (!gdi_dc_) {
            gdi_dc_ = GetDC(nullptr);
            gdi_dc_from_getdc_ = (gdi_dc_ != nullptr);
        }
        if (!gdi_dc_) return false;
    }
    if (!mem_dc_) {
        mem_dc_ = CreateCompatibleDC(gdi_dc_);
        if (!mem_dc_) return false;
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
        if (!bmp_) return false;
        old_bmp_ = static_cast<HBITMAP>(SelectObject(mem_dc_, bmp_));
        dibW_ = vw; dibH_ = vh;
    }

    // 使用虚拟桌面原点，避免有负坐标显示器时截取到错误区域。
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (!BitBlt(mem_dc_, 0, 0, vw, vh, gdi_dc_, vx, vy, SRCCOPY | CAPTUREBLT)) {
        // GetDIBits 仅会读回当前 DIB 内容，不能作为屏幕采集失败后的替代方案。
        log::Warn("GDI BitBlt failed: " + std::to_string(GetLastError()));
        return false;
    }

    // 确定输出区域:全部虚拟屏幕或指定显示器。
    int ox = 0, oy = 0, ow = vw, oh = vh;
    const int selectedMonitor = selectedMonitor_.load();
    if (selectedMonitor >= 0 && selectedMonitor < static_cast<int>(monitors_.size())) {
        const auto& m = monitors_[selectedMonitor];
        ox = m.x; oy = m.y; ow = m.w; oh = m.h;
    }

    // 从 DIB 裁剪并缩放到适合低延迟推流的尺寸。
    const auto* src = static_cast<const uint8_t*>(bits_);
    CopyRegionToFrame(src, static_cast<size_t>(vw) * 4, ox, oy, ow, oh, out);
    width_ = out.width;
    height_ = out.height;
    return true;
}

void Capture::CopyRegionToFrame(const uint8_t* source, size_t sourceStrideBytes,
                                int x, int y, int width, int height,
                                CapturedFrame& out) const {
    const double scale = std::min(1.0, std::min(
        static_cast<double>(kMaxStreamWidth) / width,
        static_cast<double>(kMaxStreamHeight) / height));
    const int outputWidth = std::max(1, static_cast<int>(width * scale));
    const int outputHeight = std::max(1, static_cast<int>(height * scale));

    out.width = outputWidth;
    out.height = outputHeight;
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

    // 最近邻缩放避免额外依赖与大缓冲，优先保障远控的低延迟。
    for (int outputY = 0; outputY < outputHeight; ++outputY) {
        const int sourceY = y + (outputY * height / outputHeight);
        uint8_t* destination = out.data.data() + static_cast<size_t>(outputY) * outputWidth * 4;
        for (int outputX = 0; outputX < outputWidth; ++outputX) {
            const int sourceX = x + (outputX * width / outputWidth);
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
