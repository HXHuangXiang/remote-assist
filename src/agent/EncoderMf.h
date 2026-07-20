#pragma once

#include <windows.h>
#include <mftransform.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace remote_assist {

// 一段编码输出。一帧可能由多个 EncodedChunk 组成(例如 keyframe 前置 SPS/PPS/IDR)。
struct EncodedChunk {
    std::vector<uint8_t> data;  // Annex-B NAL(已含 00 00 00 01 起始码)
    bool isKey = false;
};

// H.264 编码器(Media Foundation Transform,系统自带,零外部依赖)。
// 输入 BGRA 帧,内部转 NV12 后编码,输出 Annex-B NAL。
// 首个 keyframe 输出会包含 SPS/PPS/IDR,可直接送 WebCodecs 的 VideoDecoder(codec="avc3.*")。
class EncoderMf {
public:
    EncoderMf() = default;
    ~EncoderMf();

    EncoderMf(const EncoderMf&) = delete;
    EncoderMf& operator=(const EncoderMf&) = delete;

    bool Init(int width, int height, int fps, int bitrateBps);
    bool Encode(const uint8_t* bgra, std::vector<EncodedChunk>& out);

    // 返回适合 WebCodecs 的 codec 字符串(基于首个 SPS 解析),未编码前为空。
    std::string CodecString() const { return codecString_; }

    void Release();

private:
    bool ConfigureEncoder(int width, int height, int fps, int bitrateBps);
    bool PumpOutput(std::vector<EncodedChunk>& out);

    Microsoft::WRL::ComPtr<IMFTransform> enc_;
    Microsoft::WRL::ComPtr<IMFSample> inSample_;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuf_;
    Microsoft::WRL::ComPtr<IMFSample> outSample_;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf_;

    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int64_t frameIndex_ = 0;
    bool configured_ = false;
    std::string codecString_;
};

}  // namespace remote_assist

