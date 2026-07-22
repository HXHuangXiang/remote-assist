#pragma once

#include <windows.h>
#include <codecapi.h>
#include <strmif.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace remote_assist {

struct EncodedChunk {
    std::vector<uint8_t> data;
    bool isKey = true;
    // WebCodecs 使用微秒时间戳，将编码输出与浏览器绘制确认一一对应。
    uint64_t timestampUs = 0;
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
    bool Init(int width, int height, int fps, int bitrateBps);
    // bgraStrideBytes 允许 DXGI staging texture 的对齐行距，避免未缩放画面额外拷贝到
    // 紧凑 CPU 缓冲区。
    bool Encode(const uint8_t* bgra, size_t bgraStrideBytes, std::vector<EncodedChunk>& out);
    std::string CodecString() const;
    // 由采集线程在新控制端、切屏或解码恢复后调用。请求会保持到编码器实际输出
    // IDR 为止，避免把无法独立解码的增量帧作为新流首帧发送。
    void RequestKeyFrame();
    // 在支持 ICodecAPI 的 H.264 MFT 上动态调整目标码率。硬件驱动若仅支持初始化
    // 时设置会返回 false，调用方应安全退回到仅调整采集 FPS。
    bool UpdateBitrate(int bitrateBps);
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
    bool DrainH264(std::vector<EncodedChunk>& out);
    bool EncodeJpeg(const uint8_t* bgra, size_t bgraStrideBytes, std::vector<EncodedChunk>& out);
    void ReleaseH264();
    bool ForceH264KeyFrame();
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
    // caller-allocated 输出模式下复用 MFT 输出缓冲。ProcessOutput 返回后立即复制
    // NAL 数据，因此下次调用前可安全复用；输入样本可能被 MFT 异步持有，不能同样
    // 复用，仍维持每帧独立输入缓冲以保证正确性。
    Microsoft::WRL::ComPtr<IMFSample> h264OutputSample_;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> h264OutputBuffer_;
    DWORD h264OutputBufferSize_ = 0;
    MFT_OUTPUT_STREAM_INFO outputStreamInfo_{};
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int bitrateBps_ = 4'000'000;
    int quality_ = 75;
    uint64_t frameIndex_ = 0;
    bool mfStarted_ = false;
    bool hardwareH264_ = false;
    EncoderMode mode_ = EncoderMode::kJpeg;
    bool configured_ = false;
    // 缓存最新 SPS/PPS。某些 MFT 将其与 IDR 拆成不同 sample 输出，重连时必须
    // 将它们与关键帧一起发送，浏览器才能立即建立解码器状态。
    std::vector<uint8_t> h264Sps_;
    std::vector<uint8_t> h264Pps_;
    uint32_t h264CodecProfile_ = 0x42E01E;  // Baseline 3.0 的保守初值。
    bool keyFrameRequested_ = true;
};

}  // namespace remote_assist
