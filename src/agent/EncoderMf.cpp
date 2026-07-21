#include "agent/EncoderMf.h"

#include "common/Log.h"

#include <codecapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace remote_assist {

namespace {

// BT.601 BGRA -> NV12。NV12 = Y plane(w*h) + interleaved UV plane(w*h/2)。
void BgraToNv12(const uint8_t* bgra, int w, int h, uint8_t* nv12) {
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + static_cast<size_t>(w) * h;
    for (int i = 0; i < w * h; ++i) {
        const uint8_t b = bgra[i * 4 + 0];
        const uint8_t g = bgra[i * 4 + 1];
        const uint8_t r = bgra[i * 4 + 2];
        const int y = (66 * r + 129 * g + 25 * b + 128) >> 8;
        yPlane[i] = static_cast<uint8_t>(y + 16);
    }
    for (int y = 0; y < h - 1; y += 2) {
        for (int x = 0; x < w - 1; x += 2) {
            const uint8_t* p00 = bgra + ((y + 0) * w + (x + 0)) * 4;
            const uint8_t* p01 = bgra + ((y + 0) * w + (x + 1)) * 4;
            const uint8_t* p10 = bgra + ((y + 1) * w + (x + 0)) * 4;
            const uint8_t* p11 = bgra + ((y + 1) * w + (x + 1)) * 4;
            const int b = (p00[0] + p01[0] + p10[0] + p11[0]) / 4;
            const int g = (p00[1] + p01[1] + p10[1] + p11[1]) / 4;
            const int r = (p00[2] + p01[2] + p10[2] + p11[2]) / 4;
            const int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            const int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            const size_t uvIdx = static_cast<size_t>(y / 2) * w + (x & ~1);
            uvPlane[uvIdx + 0] = static_cast<uint8_t>(u);
            uvPlane[uvIdx + 1] = static_cast<uint8_t>(v);
        }
    }
}

// 解析 Annex-B 流里的第一个 SPS,构造 codec 字符串 "avc3.PPLLvv"(profile/constraint/level)。
std::string MakeCodecString(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i + 4 <= len) {
        // 找起始码 00 00 00 01 或 00 00 01
        size_t scLen = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            scLen = 4;
        } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            scLen = 3;
        } else {
            ++i;
            continue;
        }
        const size_t nalStart = i + scLen;
        if (nalStart >= len) {
            break;
        }
        const uint8_t nalType = data[nalStart] & 0x1F;
        if (nalType == 7 && nalStart + 3 < len) {
            // SPS: profile_idc, constraint, level_idc
            const uint8_t profile = data[nalStart + 1];
            const uint8_t constraint = data[nalStart + 2];
            const uint8_t level = data[nalStart + 3];
            char buf[16];
            std::snprintf(buf, sizeof(buf), "avc3.%02X%02X%02X",
                          profile, constraint, level);
            return buf;
        }
        i = nalStart + 1;
    }
    return "avc3.42E01E";  // 默认 Baseline 3.0
}

// 把 MF 输出(MP4 4 字节长度前缀 NAL)转成 Annex-B,追加到 chunk.data。
// 同时检测是否含 IDR(NAL type 5),返回 chunk.isKey。
bool AnnexBFromMp4(const uint8_t* data, size_t len, EncodedChunk& chunk) {
    static const uint8_t kStart[] = {0, 0, 0, 1};
    size_t i = 0;
    bool hasIdr = false;
    while (i + 4 <= len) {
        uint32_t n = static_cast<uint32_t>(data[i]) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     (static_cast<uint32_t>(data[i + 2]) << 16) |
                     (static_cast<uint32_t>(data[i + 3]) << 24);
        i += 4;
        if (n == 0 || i + n > len) {
            break;
        }
        const uint8_t nalType = data[i] & 0x1F;
        if (nalType == 5) {
            hasIdr = true;
        }
        chunk.data.insert(chunk.data.end(), kStart, kStart + 4);
        chunk.data.insert(chunk.data.end(), data + i, data + i + n);
        i += n;
    }
    chunk.isKey = hasIdr;
    return !chunk.data.empty();
}

}  // namespace

EncoderMf::~EncoderMf() {
    Release();
}

void EncoderMf::Release() {
    outBuf_.Reset();
    outSample_.Reset();
    inBuf_.Reset();
    inSample_.Reset();
    enc_.Reset();
    configured_ = false;
}

bool EncoderMf::Init(int width, int height, int fps, int bitrateBps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
   HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
       &inInfo, &outInfo, &acts, &count);
    if (FAILED(hr) || count == 0) {
        log::Error("MFTEnumEx H.264 encoder not found");
        return false;
    }
    hr = acts[0]->ActivateObject(IID_PPV_ARGS(&enc_));
    for (UINT32 i = 0; i < count; ++i) {
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    if (FAILED(hr)) {
        log::Error("ActivateObject encoder failed: " + std::to_string(hr));
        return false;
    }
    return ConfigureEncoder(width, height, fps, bitrateBps);
}

bool EncoderMf::ConfigureEncoder(int width, int height, int fps, int bitrateBps) {
    using Microsoft::WRL::ComPtr;
    // MF 编码器类型协商:先设最小输出类型(major+subtype)打破鸡生蛋,
    // 再枚举/设输入类型,最后用 GetOutputAvailableType 重设完整输出类型。
    ComPtr<IMFMediaType> minOut;
    MFCreateMediaType(&minOut);
    minOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    minOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    HRESULT hr = enc_->SetOutputType(0, minOut.Get(), 0);
    if (FAILED(hr)) {
        log::Error("SetOutputType(minimal) failed hr=" + std::to_string(hr));
        return false;
    }
    log::Info("output type (minimal) set OK");

    // Step 2: 输出已设(最小),枚举输入类型找 NV12 或兼容格式。
    ComPtr<IMFMediaType> inMt;
    bool inFound = false;
    for (DWORD i = 0; i < 32; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hrt = enc_->GetInputAvailableType(0, i, &candidate);
        if (hrt != S_OK) break;
        GUID sub = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &sub);
        log::Info("input type " + std::to_string(i) + " checked");
        if (sub == MFVideoFormat_NV12 || sub == MFVideoFormat_RGB32 ||
            sub == MFVideoFormat_RGB24 || sub == MFVideoFormat_YUY2) {
            MFCreateMediaType(&inMt);
            candidate->CopyAllItems(inMt.Get());
            inFound = true;
            log::Info("input type idx=" + std::to_string(i) + " accepted");
            break;
        }
    }
    if (!inFound) {
        MFCreateMediaType(&inMt);
        inMt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inMt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        log::Warn("no NV12 input from encoder, using custom");
    }
    MFSetAttributeSize(inMt.Get(), MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    MFSetAttributeRatio(inMt.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    inMt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = enc_->SetInputType(0, inMt.Get(), 0);
    if (FAILED(hr)) {
        log::Error("SetInputType failed hr=" + std::to_string(hr));
        return false;
    }
    log::Info("input type set OK");

    // Step 3: 输入已设,用 GetOutputAvailableType 重设完整输出类型。
    ComPtr<IMFMediaType> outMt;
    bool outFound = false;
    for (DWORD i = 0; i < 32; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hrt = enc_->GetOutputAvailableType(0, i, &candidate);
        if (hrt != S_OK) break;
        GUID sub = GUID_NULL;
        candidate->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_H264) {
            MFCreateMediaType(&outMt);
            candidate->CopyAllItems(outMt.Get());
            outFound = true;
            log::Info("output type idx=" + std::to_string(i) + " is H.264");
            break;
        }
    }
    if (!outFound) {
        // GetOutputAvailableType 没返回类型,沿用最小输出类型,仅补充帧尺寸/帧率。
        outMt = minOut;
        log::Warn("no H.264 output from GetOutputAvailableType, keeping minimal+attrs");
    }
    MFSetAttributeSize(outMt.Get(), MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
    MFSetAttributeRatio(outMt.Get(), MF_MT_FRAME_RATE, (UINT32)fps, 1);
    outMt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = enc_->SetOutputType(0, outMt.Get(), 0);
    if (FAILED(hr)) {
        log::Warn("SetOutputType(full) failed hr=" + std::to_string(hr) + ", keeping minimal");
    }

    // 尽力配置码率与码控模式(通过 MFT 属性存储设置 codec 参数,避免引入 ICodecAPI/strmif.h)。
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(enc_->GetAttributes(&attrs))) {
        attrs->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, static_cast<UINT32>(bitrateBps));
        attrs->SetUINT32(CODECAPI_AVEncCommonRateControlMode, 0);  // 0 = CBR
    }

    const DWORD nv12Size = static_cast<DWORD>(width * height * 3 / 2);
    if (FAILED(MFCreateSample(&inSample_))) {
        return false;
    }
    if (FAILED(MFCreateMemoryBuffer(nv12Size, &inBuf_))) {
        return false;
    }
    inSample_->AddBuffer(inBuf_.Get());

    const DWORD outSize = static_cast<DWORD>(width * height * 2);
    if (FAILED(MFCreateSample(&outSample_))) {
        return false;
    }
    if (FAILED(MFCreateMemoryBuffer(outSize, &outBuf_))) {
        return false;
    }
    outSample_->AddBuffer(outBuf_.Get());

    configured_ = true;
    log::Info("encoder configured: " + std::to_string(width) + "x" +
              std::to_string(height) + "@" + std::to_string(fps) +
              " bitrate=" + std::to_string(bitrateBps));
    return true;
}

bool EncoderMf::Encode(const uint8_t* bgra, std::vector<EncodedChunk>& out) {
    if (!configured_) {
        return false;
    }
    DWORD maxLen = 0;
    DWORD curLen = 0;
    BYTE* p = nullptr;
    if (FAILED(inBuf_->Lock(&p, &maxLen, &curLen))) {
        return false;
    }
    BgraToNv12(bgra, width_, height_, p);
    inBuf_->Unlock();
    inBuf_->SetCurrentLength(static_cast<DWORD>(width_ * height_ * 3 / 2));

    const LONGLONG ts = static_cast<LONGLONG>(frameIndex_++) * 10000000LL / fps_;  // 100ns units
    inSample_->SetSampleTime(ts);
    inSample_->SetSampleDuration(10000000LL / fps_);

    HRESULT hr = enc_->ProcessInput(0, inSample_.Get(), 0);
    if (FAILED(hr)) {
        log::Warn("ProcessInput failed: " + std::to_string(hr));
        return false;
    }
    return PumpOutput(out);
}

bool EncoderMf::PumpOutput(std::vector<EncodedChunk>& out) {
    MFT_OUTPUT_DATA_BUFFER obd{};
    obd.pSample = outSample_.Get();  // 复用同一输出 sample
    bool gotAny = false;
    for (;;) {
        DWORD status = 0;
        HRESULT hr = enc_->ProcessOutput(0, 1, &obd, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (FAILED(hr)) {
            log::Warn("ProcessOutput failed: " + std::to_string(hr));
            break;
        }
        if (obd.pSample) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(obd.pSample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD maxL = 0;
                DWORD curL = 0;
                if (SUCCEEDED(buf->Lock(&data, &maxL, &curL)) && curL > 0) {
                    EncodedChunk chunk;
                    if (AnnexBFromMp4(data, curL, chunk)) {
                        if (codecString_.empty()) {
                            codecString_ = MakeCodecString(chunk.data.data(),
                                                           chunk.data.size());
                        }
                        out.push_back(std::move(chunk));
                        gotAny = true;
                    }
                    buf->Unlock();
                }
            }
            // 复用 outSample_,释放 MFT 可能归还的资源
            if (obd.pSample != outSample_.Get() && obd.pSample) {
                obd.pSample->Release();
                obd.pSample = outSample_.Get();
            }
        }
    }
    return gotAny;
}

}  // namespace remote_assist
