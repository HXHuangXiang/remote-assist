#pragma once

#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <strmif.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace remote_assist {

struct EncodedChunk {
    std::vector<uint8_t> data;
    bool isKey = true;
    // WebCodecs 使用微秒时间戳，将编码输出与浏览器绘制确认一一对应。
    uint64_t timestampUs = 0;
};

// CPU H.264 输入路径的分段耗时。MFT 实际编码与 Drain 已由 Agent 的 encode_avg_ms
// 覆盖；这组数据专门识别 BGRA->NV12 转换和每帧 MF 输入对象创建是否成为回退热点。
struct EncoderFrameTiming {
    uint64_t bgraToNv12Us = 0;
    uint64_t mfInputPreparationUs = 0;
    uint64_t d3dInputWrapUs = 0;
};

enum class EncoderMode {
    kJpeg,
    kH264,
};

// 编码器优先使用系统 H.264 MFT（硬件 MFT 优先、微软软件 MFT 兜底）；若当前
// 驱动或系统不提供可用 H.264 编码器则自动退回现有 WIC JPEG，保证锁屏 GDI
// 采集路径仍可用。
class EncoderMf {
public:
    EncoderMf() = default;
    ~EncoderMf();
    EncoderMf(const EncoderMf&) = delete;
    EncoderMf& operator=(const EncoderMf&) = delete;
    bool Init(int width, int height, int fps, int bitrateBps,
              ID3D11Device* d3dDevice = nullptr);
    // bgraStrideBytes 允许 DXGI staging texture 的对齐行距，避免未缩放画面额外拷贝到
    // 紧凑 CPU 缓冲区。
    bool Encode(const uint8_t* bgra, size_t bgraStrideBytes, std::vector<EncodedChunk>& out);
    // 同一 D3D11 设备上的 NV12 surface 可直接输入硬件 MFT，跳过 CPU 读回和
    // BGRA->NV12 转换。失败由调用方重新初始化为已有 CPU 路径。
    bool EncodeD3D11(ID3D11Texture2D* nv12Texture, std::vector<EncodedChunk>& out);
    // 局部刷新使用独立 JPEG 图块，不能复用当前 H.264 的帧尺寸状态。该方法只
    // 编码指定的紧凑 BGRA 图像，不会推进 H.264 的参考帧或时间线。
    bool EncodeJpegTile(int width, int height, const uint8_t* bgra, size_t bgraStrideBytes,
                        std::vector<uint8_t>& out);
    bool CanEncodeD3D11() const { return d3dInputEnabled_; }
    // 仅由编码所在的采集线程在 Encode/EncodeD3D11 返回后读取。
    const EncoderFrameTiming& LastFrameTiming() const { return lastFrameTiming_; }
    std::string CodecString() const;
    // 由采集线程在新控制端、切屏或解码恢复后调用。请求会保持到编码器实际输出
    // IDR 为止，避免把无法独立解码的增量帧作为新流首帧发送。
    void RequestKeyFrame();
    // 在支持 ICodecAPI 的 H.264 MFT 上动态调整目标码率。硬件驱动若仅支持初始化
    // 时设置会返回 false，调用方应安全退回到仅调整采集 FPS。
    bool UpdateBitrate(int bitrateBps);
    // 当前 H.264 MFT 在首帧恢复窗口内持续不给出 IDR 时，主动切换到已经验证的
    // JPEG 路径，避免控制端因一直丢弃预测帧而长期黑屏。仅能由拥有 MFT 的采集
    // 线程调用；下一次重建编码器后仍会重新尝试系统 H.264。
    bool ForceJpegFallback();
    // H.264 SPS 的 profile_idc/profile_compatibility/level_idc，格式为 0xPPCCLL。
    // 仅由采集线程读取；跨线程下发配置时应由 Agent 保存其副本。
    uint32_t H264CodecProfile() const { return h264CodecProfile_; }
    bool IsH264() const { return mode_ == EncoderMode::kH264; }
    void Release();
private:
    bool InitH264();
    bool InitJpeg();
    bool TryCreateHardwareH264();
    bool TryCreateSoftwareH264();
    bool ConfigureH264Transform();
    bool ConfigureH264OutputType();
    bool EncodeH264(const uint8_t* bgra, size_t bgraStrideBytes, std::vector<EncodedChunk>& out);
    bool SubmitH264Sample(IMFSample* inputSample, std::vector<EncodedChunk>& out);
    bool DrainH264(std::vector<EncodedChunk>& out);
    bool EncodeJpeg(const uint8_t* bgra, size_t bgraStrideBytes, std::vector<EncodedChunk>& out);
    bool EncodeJpegImage(int width, int height, const uint8_t* bgra, size_t bgraStrideBytes,
                         std::vector<uint8_t>& out);
    bool EnsureWicFactory();
    DWORD H264OutputBufferTargetSize() const;
    // 采集帧率会被 Agent 根据网络和编码压力动态下调，因此不能继续以初始化时的
    // fps_ 推导 PTS。使用单调时钟为 MFT/WebCodecs 提供真实、严格递增的时间基。
    LONGLONG NextInputTimestamp100Ns();
    LONGLONG InputDuration100Ns(LONGLONG timestamp100Ns) const;
    void ReleaseH264();
    bool ForceH264KeyFrame();
    bool TryEnableD3D11Input();
    void ConvertBgraToNv12(const uint8_t* bgra, size_t bgraStrideBytes, uint8_t* nv12) const;
    void CacheH264ParameterSets(const std::vector<uint8_t>& data);
    void PrependCachedParameterSets(EncodedChunk& chunk) const;
    static void CoalesceH264AccessUnits(std::vector<EncodedChunk>& chunks);
    static bool NormalizeH264ToAnnexB(const uint8_t* source, size_t len,
                                      std::vector<uint8_t>& destination);
    static bool HasH264NalType(const std::vector<uint8_t>& data, uint8_t nalType);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<IMFTransform> h264Mft_;
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    // caller-allocated 输出模式下复用 MFT 输出缓冲。ProcessOutput 返回后立即复制
    // NAL 数据，因此下次调用前可安全复用；输入样本可能被 MFT 异步持有，不能同样
    // 复用，仍维持每帧独立输入缓冲以保证正确性。
    Microsoft::WRL::ComPtr<IMFSample> h264OutputSample_;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> h264OutputBuffer_;
    DWORD h264OutputBufferSize_ = 0;
    // 运行期收到 MF_E_BUFFERTOOSMALL 后抬高的容量下限；分辨率/码率变化时仍保留
    // 已验证的安全容量，避免关键帧反复触发同一失败。
    DWORD h264OutputBufferFloorSize_ = 0;
    MFT_OUTPUT_STREAM_INFO outputStreamInfo_{};
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int bitrateBps_ = 4'000'000;
    int quality_ = 75;
    uint64_t frameIndex_ = 0;
    std::chrono::steady_clock::time_point timelineStartedAt_{};
    LONGLONG lastInputTimestamp100Ns_ = -1;
    // 定期 IDR 必须以真实时间而非“输入帧数 / 初始 FPS”决定，否则自适应降帧后
    // 连接恢复会等待远超预期的关键帧间隔。
    LONGLONG lastPeriodicKeyFrameRequest100Ns_ =
        std::numeric_limits<LONGLONG>::min() / 2;
    bool mfStarted_ = false;
    bool hardwareH264_ = false;
    bool d3dInputEnabled_ = false;
    EncoderMode mode_ = EncoderMode::kJpeg;
    bool configured_ = false;
    // 缓存最新 SPS/PPS。某些 MFT 将其与 IDR 拆成不同 sample 输出，重连时必须
    // 将它们与关键帧一起发送，浏览器才能立即建立解码器状态。
    std::vector<uint8_t> h264Sps_;
    std::vector<uint8_t> h264Pps_;
    uint32_t h264CodecProfile_ = 0x42E01E;  // Baseline 3.0 的保守初值。
    bool keyFrameRequested_ = true;
    EncoderFrameTiming lastFrameTiming_{};
};

}  // namespace remote_assist
