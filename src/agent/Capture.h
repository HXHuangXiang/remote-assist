#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace remote_assist {

// 一帧采集结果。当前统一输出 BGRA(每像素 4 字节,BGRA 字节序),
// 由 EncoderMf 内部转 NV12 后送 H.264 编码。
struct CapturedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;  // width*height*4 字节 BGRA
};

// 屏幕采集:主路径 DXGI Desktop Duplication;失败(锁屏桌面等)回退 GDI BitBlt。
class Capture {
public:
    Capture() = default;
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    bool Init();

    // 采集一帧。无变化或失败返回 false(调用方可决定跳过/重试)。
    bool CaptureFrame(CapturedFrame& out);

private:
    bool InitDXGI();
    bool CaptureDXGI(CapturedFrame& out);
    bool CaptureGDI(CapturedFrame& out);
    void ReleaseAll();

    // DXGI
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

    // GDI
    HDC gdi_dc_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HBITMAP old_bmp_ = nullptr;
    void* bits_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    bool use_gdi_ = false;
};

}  // namespace remote_assist

