#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace remote_assist {

struct CapturedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
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
    bool CaptureFrame(CapturedFrame& out);

    // 多显示器:枚举显示器列表。-1=全部(虚拟屏幕),>=0=指定显示器。
    void EnumMonitors();
    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM);
    const std::vector<MonitorInfo>& Monitors() const { return monitors_; }
    void SetMonitor(int index) { selectedMonitor_ = index; }

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

    int dibW_ = 0;   // DIB 宽(虚拟屏幕宽)
    int dibH_ = 0;   // DIB 高(虚拟屏幕高)

    // 多显示器
    std::vector<MonitorInfo> monitors_;
    int selectedMonitor_ = -1;  // -1=全部

    int width_ = 0;
    int height_ = 0;
    bool use_gdi_ = false;
};

}  // namespace remote_assist
