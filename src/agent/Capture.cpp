#include "agent/Capture.h"

#include "common/Log.h"

#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

namespace remote_assist {

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
    if (gdi_dc_) { DeleteDC(gdi_dc_); gdi_dc_ = nullptr; }
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
    EnumMonitors();
    if (InitDXGI()) {
        use_gdi_ = false;
        log::Info("capture: DXGI path ready");
        return true;
    }
    log::Warn("capture: DXGI init failed, fallback to GDI");
    use_gdi_ = true;
    return true;
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

bool Capture::CaptureFrame(CapturedFrame& out) {
    if (use_gdi_) return CaptureGDI(out);
    if (CaptureDXGI(out)) return true;
    return CaptureGDI(out);
}

bool Capture::CaptureDXGI(CapturedFrame& out) {
    if (!dup_) return false;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    Microsoft::WRL::ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(50, &fi, &res);
    if (FAILED(hr)) return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) { dup_->ReleaseFrame(); return false; }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (!staging_) {
        D3D11_TEXTURE2D_DESC s = desc;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        if (FAILED(d3d_->CreateTexture2D(&s, nullptr, &staging_))) { dup_->ReleaseFrame(); return false; }
    }
    ctx_->CopyResource(staging_.Get(), tex.Get());
    dup_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    out.width = w; out.height = h;
    out.data.resize(static_cast<size_t>(w) * h * 4);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < h; ++y)
        std::memcpy(out.data.data() + static_cast<size_t>(y) * w * 4,
                    src + static_cast<size_t>(y) * mapped.RowPitch, static_cast<size_t>(w) * 4);
    ctx_->Unmap(staging_.Get(), 0);
    width_ = w; height_ = h;
    return true;
}

bool Capture::CaptureGDI(CapturedFrame& out) {
    // 虚拟屏幕尺寸(涵盖所有显示器)。
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    // 创建/复用 DC 和 DIB(DIB 尺寸 = 虚拟屏幕)。
    if (!gdi_dc_) {
        gdi_dc_ = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        if (!gdi_dc_) gdi_dc_ = GetDC(nullptr);
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

    // BitBlt 主路径;失败用 GetDIBits 回退。
    if (!BitBlt(mem_dc_, 0, 0, vw, vh, gdi_dc_, 0, 0, SRCCOPY)) {
        BITMAPINFO bi2{};
        bi2.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi2.bmiHeader.biWidth = vw;
        bi2.bmiHeader.biHeight = -vh;
        bi2.bmiHeader.biPlanes = 1;
        bi2.bmiHeader.biBitCount = 32;
        bi2.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(gdi_dc_, bmp_, 0, vh, bits_, &bi2, DIB_RGB_COLORS) == 0) return false;
    }

    // 确定输出区域:全部虚拟屏幕或指定显示器。
    int ox = 0, oy = 0, ow = vw, oh = vh;
    if (selectedMonitor_ >= 0 && selectedMonitor_ < static_cast<int>(monitors_.size())) {
        const auto& m = monitors_[selectedMonitor_];
        ox = m.x; oy = m.y; ow = m.w; oh = m.h;
    }

    // 从 DIB 裁剪出输出区域。
    const auto* src = static_cast<const uint8_t*>(bits_);
    out.width = ow; out.height = oh;
    out.data.resize(static_cast<size_t>(ow) * oh * 4);
    for (int y = 0; y < oh; ++y) {
        const uint8_t* s = src + static_cast<size_t>(oy + y) * vw * 4 + static_cast<size_t>(ox) * 4;
        std::memcpy(out.data.data() + static_cast<size_t>(y) * ow * 4, s, static_cast<size_t>(ow) * 4);
    }
    width_ = ow; height_ = oh;
    return true;
}

}  // namespace remote_assist
