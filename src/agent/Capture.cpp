#include "agent/Capture.h"

#include "common/Log.h"

#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

namespace remote_assist {

Capture::~Capture() {
    ReleaseAll();
}

void Capture::ReleaseAll() {
    staging_.Reset();
    dup_.Reset();
    output_.Reset();
    ctx_.Reset();
    d3d_.Reset();
    if (bmp_) {
        if (mem_dc_) {
            SelectObject(mem_dc_, old_bmp_);
        }
        DeleteObject(bmp_);
        bmp_ = nullptr;
    }
    if (mem_dc_) {
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (gdi_dc_) {
        DeleteDC(gdi_dc_);
        gdi_dc_ = nullptr;
    }
    bits_ = nullptr;
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

bool Capture::InitDXGI() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
        return false;
    }
    // EnumOutputs 是 IDXGIAdapter 的方法,需要先枚举适配器再枚举输出。
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(fac->EnumAdapters1(0, &adapter))) {
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIOutput> out0;
    if (FAILED(adapter->EnumOutputs(0, &out0))) {
        return false;
    }
    if (FAILED(out0.As(&output_))) {
        return false;
    }

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &d3d_, &fl, &ctx_))) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &d3d_, &fl, &ctx_))) {
            return false;
        }
    }

    if (FAILED(output_->DuplicateOutput(d3d_.Get(), &dup_))) {
        return false;
    }
    return true;
}

bool Capture::CaptureFrame(CapturedFrame& out) {
    if (use_gdi_) {
        return CaptureGDI(out);
    }
    if (CaptureDXGI(out)) {
        return true;
    }
    // DXGI 失败(常见于进入锁屏桌面),临时切 GDI。
    return CaptureGDI(out);
}

bool Capture::CaptureDXGI(CapturedFrame& out) {
    if (!dup_) {
        return false;
    }
    DXGI_OUTDUPL_FRAME_INFO fi{};
    Microsoft::WRL::ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(50, &fi, &res);
    if (FAILED(hr)) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) {
        dup_->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    if (!staging_) {
        D3D11_TEXTURE2D_DESC s = desc;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        if (FAILED(d3d_->CreateTexture2D(&s, nullptr, &staging_))) {
            dup_->ReleaseFrame();
            return false;
        }
    }

    ctx_->CopyResource(staging_.Get(), tex.Get());
    dup_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    out.width = w;
    out.height = h;
    out.data.resize(static_cast<size_t>(w) * h * 4);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < h; ++y) {
        std::memcpy(out.data.data() + static_cast<size_t>(y) * w * 4,
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(w) * 4);
    }
    ctx_->Unmap(staging_.Get(), 0);

    width_ = w;
    height_ = h;
    return true;
}

bool Capture::CaptureGDI(CapturedFrame& out) {
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    static int diagCount = 0;
    if (++diagCount % 300 == 1) {
        log::Info("CaptureGDI diag: w=" + std::to_string(w) + " h=" + std::to_string(h) +
                   " gdi_dc=" + std::to_string(gdi_dc_ != nullptr) +
                   " mem_dc=" + std::to_string(mem_dc_ != nullptr) +
                   " bmp=" + std::to_string(bmp_ != nullptr));
    }
    if (w <= 0 || h <= 0) {
        if (diagCount % 300 == 1) log::Error("CaptureGDI: screen metrics 0");
        return false;
    }

    // 用 CreateDCW 创建自有显示 DC(比 GetDC(nullptr) 更稳定,不会被系统回收)。
    if (!gdi_dc_) {
        gdi_dc_ = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        if (diagCount % 300 == 1) log::Info("CaptureGDI: CreateDCW display=" + std::to_string(gdi_dc_ != nullptr));
        if (!gdi_dc_) {
            gdi_dc_ = GetDC(nullptr);
            if (diagCount % 300 == 1) log::Info("CaptureGDI: GetDC fallback=" + std::to_string(gdi_dc_ != nullptr));
        }
    }
    if (!mem_dc_) {
        mem_dc_ = CreateCompatibleDC(gdi_dc_);
        if (diagCount % 300 == 1) log::Info("CaptureGDI: CreateCompatibleDC=" + std::to_string(mem_dc_ != nullptr));
        if (!mem_dc_) return false;
    }

    if (!bmp_ || width_ != w || height_ != h) {
        if (bmp_) {
            SelectObject(mem_dc_, old_bmp_);
            DeleteObject(bmp_);
            bmp_ = nullptr;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp_ = CreateDIBSection(mem_dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bmp_) {
            if (diagCount % 300 == 1) log::Error("CaptureGDI: CreateDIBSection failed");
            return false;
        }
        old_bmp_ = static_cast<HBITMAP>(SelectObject(mem_dc_, bmp_));
        width_ = w;
        height_ = h;
    }

    // BitBlt 主路径;失败则用 GetDIBits 回退(BitBlt 在某些桌面/驱动下返回 INVALID_HANDLE)。
    if (!BitBlt(mem_dc_, 0, 0, w, h, gdi_dc_, 0, 0, SRCCOPY)) {
        if (diagCount % 300 == 1) log::Warn("CaptureGDI: BitBlt err=" + std::to_string(GetLastError()) + ", trying GetDIBits");
        BITMAPINFO bi2{};
        bi2.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi2.bmiHeader.biWidth = w;
        bi2.bmiHeader.biHeight = -h;
        bi2.bmiHeader.biPlanes = 1;
        bi2.bmiHeader.biBitCount = 32;
        bi2.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(gdi_dc_, bmp_, 0, h, bits_, &bi2, DIB_RGB_COLORS) == 0) {
            if (diagCount % 300 == 1) log::Error("CaptureGDI: GetDIBits also failed");
            return false;
        }
    }

    out.width = w;
    out.height = h;
    out.data.assign(static_cast<uint8_t*>(bits_),
                    static_cast<uint8_t*>(bits_) + static_cast<size_t>(w) * h * 4);
    return true;
}

}  // namespace remote_assist
