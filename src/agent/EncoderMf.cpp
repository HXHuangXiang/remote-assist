#include "agent/EncoderMf.h"

#include "common/Log.h"

#include <mferror.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace remote_assist {

namespace {

constexpr LONGLONG kHundredNsPerSecond = 10'000'000;
constexpr uint64_t kMicrosecondsPerSecond = 1'000'000;
constexpr int kKeyFrameIntervalSeconds = 2;

uint8_t ClampToByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

bool HasAnnexBStartCode(const uint8_t* data, size_t len) {
    return len >= 4 && data[0] == 0 && data[1] == 0 &&
        ((data[2] == 0 && data[3] == 1) || data[2] == 1);
}

HRESULT SetCodecUInt32(ICodecAPI* codecApi, const GUID& key, ULONG value) {
    if (!codecApi) {
        return E_NOINTERFACE;
    }
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    const HRESULT hr = codecApi->SetValue(&key, &v);
    VariantClear(&v);
    return hr;
}

size_t AnnexBStartCodeLength(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 3 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 1) {
        return 3;
    }
    if (offset + 4 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 0 && data[offset + 3] == 1) {
        return 4;
    }
    return 0;
}

size_t FindNextAnnexBStartCode(const std::vector<uint8_t>& data, size_t offset) {
    for (size_t index = offset; index < data.size(); ++index) {
        if (AnnexBStartCodeLength(data, index) != 0) {
            return index;
        }
    }
    return data.size();
}

}  // namespace

// 内存 IStream,接收 WIC 编码器的 JPEG 输出。
class MemoryStream : public IStream {
public:
    MemoryStream() : ref_(1), pos_(0) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IStream) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        const size_t available = pos_ < buf_.size() ? buf_.size() - pos_ : 0;
        const ULONG n = static_cast<ULONG>(std::min<size_t>(cb, available));
        if (n) std::memcpy(pv, buf_.data() + pos_, n);
        pos_ += n;
        if (pcbRead) *pcbRead = n;
        return n == cb ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override {
        if (pos_ + cb > buf_.size()) buf_.resize(pos_ + cb);
        std::memcpy(buf_.data() + pos_, pv, cb); pos_ += cb;
        if (pcbWritten) *pcbWritten = cb;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER d, DWORD origin, ULARGE_INTEGER* p) override {
        int64_t np = static_cast<int64_t>(pos_);
        if (origin == STREAM_SEEK_SET) np = d.QuadPart;
        else if (origin == STREAM_SEEK_CUR) np += d.QuadPart;
        else np = static_cast<int64_t>(buf_.size()) + d.QuadPart;
        if (np < 0) return STG_E_INVALIDFUNCTION;
        pos_ = static_cast<size_t>(np);
        if (p) p->QuadPart = pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER s) override {
        buf_.resize(static_cast<size_t>(s.QuadPart));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* p, DWORD) override {
        if (p) { std::memset(p, 0, sizeof(*p)); p->cbSize.QuadPart = buf_.size(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
    std::vector<uint8_t> TakeData() { return std::move(buf_); }

private:
    LONG ref_;
    size_t pos_;
    std::vector<uint8_t> buf_;
};

EncoderMf::~EncoderMf() { Release(); }

bool EncoderMf::Init(int width, int height, int fps, int bitrateBps) {
    Release();
    if (width < 2 || height < 2 || (width & 1) != 0 || (height & 1) != 0 || fps <= 0) {
        log::Error("encoder received unsupported frame size");
        return false;
    }

    width_ = width;
    height_ = height;
    fps_ = std::max(1, fps);
    bitrateBps_ = std::max(100'000, bitrateBps);
    frameIndex_ = 0;
    h264Sps_.clear();
    h264Pps_.clear();
    h264CodecProfile_ = 0x42E01E;
    keyFrameRequested_ = true;

    if (InitH264()) {
        configured_ = true;
        log::Info(std::string("H.264 encoder ready (") +
                  (hardwareH264_ ? "hardware" : "software") + "): " +
                  std::to_string(width_) + "x" + std::to_string(height_) +
                  " bitrate=" + std::to_string(bitrateBps_));
        return true;
    }

    log::Warn("H.264 encoder unavailable, fallback to JPEG");
    return InitJpeg();
}

std::string EncoderMf::CodecString() const {
    if (mode_ != EncoderMode::kH264) {
        return "jpeg";
    }
    char value[16] = {};
    std::snprintf(value, sizeof(value), "avc1.%06X", h264CodecProfile_ & 0xFFFFFF);
    return value;
}

void EncoderMf::RequestKeyFrame() {
    // Encode/Drain 均在采集线程中串行执行；请求保留到看到实际 IDR，不能在
    // SetValue 成功时就清除，因为部分 MFT 存在一两帧流水线延迟。
    if (mode_ == EncoderMode::kH264) {
        keyFrameRequested_ = true;
    }
}

bool EncoderMf::InitH264() {
    const HRESULT startup = MFStartup(MF_VERSION);
    if (FAILED(startup)) {
        log::Warn("MFStartup failed hr=" + std::to_string(startup));
        return false;
    }
    mfStarted_ = true;

    if (TryCreateHardwareH264()) {
        mode_ = EncoderMode::kH264;
        hardwareH264_ = true;
        return true;
    }
    if (TryCreateSoftwareH264()) {
        mode_ = EncoderMode::kH264;
        hardwareH264_ = false;
        return true;
    }

    ReleaseH264();
    return false;
}

bool EncoderMf::TryCreateHardwareH264() {
    MFT_REGISTER_TYPE_INFO inputType{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outputType{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType, &outputType, &activations, &count);
    if (FAILED(hr) || count == 0) {
        if (activations) CoTaskMemFree(activations);
        return false;
    }

    bool configured = false;
    for (UINT32 index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMFTransform> candidate;
        if (SUCCEEDED(activations[index]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
            h264Mft_ = std::move(candidate);
            if (ConfigureH264Transform()) {
                configured = true;
                break;
            }
            h264Mft_.Reset();
            codecApi_.Reset();
        }
    }
    for (UINT32 index = 0; index < count; ++index) {
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    return configured;
}

bool EncoderMf::TryCreateSoftwareH264() {
    Microsoft::WRL::ComPtr<IMFTransform> candidate;
    const HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr,
                                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&candidate));
    if (FAILED(hr)) {
        log::Warn("software H.264 MFT unavailable hr=" + std::to_string(hr));
        return false;
    }
    h264Mft_ = std::move(candidate);
    if (ConfigureH264Transform()) {
        return true;
    }
    h264Mft_.Reset();
    codecApi_.Reset();
    return false;
}

bool EncoderMf::ConfigureH264Transform() {
    if (!h264Mft_.Get()) {
        return false;
    }
    h264Mft_.As(&codecApi_);
    // 常见硬件 MFT 支持码率属性；不支持时仍由 media type 的平均码率兜底。
    // Windows SDK 并没有通用的 H.264 profile CodecAPI 属性，因此不能假定编码器
    // 一定是 Baseline；实际 profile 由后续 SPS 解析后下发给浏览器。
    const HRESULT bitrateResult = SetCodecUInt32(codecApi_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                                                  static_cast<ULONG>(bitrateBps_));
    if (codecApi_.Get() && FAILED(bitrateResult)) {
        log::Warn("H.264 MFT ignored requested bitrate hr=" + std::to_string(bitrateResult));
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = h264Mft_->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        return false;
    }
    if (!ConfigureH264OutputType()) {
        return false;
    }
    h264Mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    h264Mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    h264Mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool EncoderMf::ConfigureH264OutputType() {
    for (DWORD index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IMFMediaType> outputType;
        const HRESULT getType = h264Mft_->GetOutputAvailableType(0, index, &outputType);
        if (getType == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(getType)) {
            continue;
        }
        GUID subtype = GUID_NULL;
        if (FAILED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_H264) {
            continue;
        }
        outputType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateBps_));
        outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps_, 1);
        if (SUCCEEDED(h264Mft_->SetOutputType(0, outputType.Get(), 0)) &&
            SUCCEEDED(h264Mft_->GetOutputStreamInfo(0, &outputStreamInfo_))) {
            return true;
        }
    }
    return false;
}

bool EncoderMf::ForceH264KeyFrame() {
    if (!codecApi_.Get()) {
        return false;
    }
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    const HRESULT hr = codecApi_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
    VariantClear(&value);
    return SUCCEEDED(hr);
}

void EncoderMf::ConvertBgraToNv12(const uint8_t* bgra, uint8_t* nv12) const {
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + static_cast<size_t>(width_) * height_;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const uint8_t* pixel = bgra + (static_cast<size_t>(y) * width_ + x) * 4;
            const int b = pixel[0];
            const int g = pixel[1];
            const int r = pixel[2];
            yPlane[static_cast<size_t>(y) * width_ + x] =
                ClampToByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int y = 0; y < height_; y += 2) {
        for (int x = 0; x < width_; x += 2) {
            int r = 0;
            int g = 0;
            int b = 0;
            for (int row = 0; row < 2; ++row) {
                for (int column = 0; column < 2; ++column) {
                    const uint8_t* pixel = bgra +
                        (static_cast<size_t>(y + row) * width_ + x + column) * 4;
                    b += pixel[0];
                    g += pixel[1];
                    r += pixel[2];
                }
            }
            r /= 4;
            g /= 4;
            b /= 4;
            uint8_t* uv = uvPlane + static_cast<size_t>(y / 2) * width_ + x;
            uv[0] = ClampToByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[1] = ClampToByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

bool EncoderMf::NormalizeH264ToAnnexB(const uint8_t* source, size_t len,
                                      std::vector<uint8_t>& destination) {
    if (!source || len == 0) {
        return false;
    }
    if (HasAnnexBStartCode(source, len)) {
        destination.assign(source, source + len);
        return true;
    }

    destination.clear();
    size_t offset = 0;
    while (offset + 4 <= len) {
        const uint32_t nalLength = (static_cast<uint32_t>(source[offset]) << 24) |
            (static_cast<uint32_t>(source[offset + 1]) << 16) |
            (static_cast<uint32_t>(source[offset + 2]) << 8) |
            static_cast<uint32_t>(source[offset + 3]);
        offset += 4;
        if (nalLength == 0 || nalLength > len - offset) {
            destination.clear();
            return false;
        }
        destination.insert(destination.end(), {0, 0, 0, 1});
        destination.insert(destination.end(), source + offset, source + offset + nalLength);
        offset += nalLength;
    }
    if (offset != len || destination.empty()) {
        destination.clear();
        return false;
    }
    return true;
}

bool EncoderMf::HasH264NalType(const std::vector<uint8_t>& data, uint8_t nalType) {
    for (size_t index = 0; index + 4 < data.size(); ++index) {
        size_t nalStart = 0;
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
            nalStart = index + 3;
        } else if (index + 4 < data.size() && data[index] == 0 && data[index + 1] == 0 &&
                   data[index + 2] == 0 && data[index + 3] == 1) {
            nalStart = index + 4;
        } else {
            continue;
        }
        if (nalStart < data.size() && (data[nalStart] & 0x1F) == nalType) {
            return true;
        }
    }
    return false;
}

void EncoderMf::CacheH264ParameterSets(const std::vector<uint8_t>& data) {
    for (size_t start = FindNextAnnexBStartCode(data, 0); start < data.size();) {
        const size_t startCodeLength = AnnexBStartCodeLength(data, start);
        const size_t nalStart = start + startCodeLength;
        const size_t nextStart = FindNextAnnexBStartCode(data, nalStart);
        const size_t nalEnd = nextStart < data.size() ? nextStart : data.size();
        if (nalStart < nalEnd) {
            const uint8_t nalType = data[nalStart] & 0x1F;
            if (nalType == 7 || nalType == 8) {
                std::vector<uint8_t> parameterSet{0, 0, 0, 1};
                parameterSet.insert(parameterSet.end(), data.begin() + nalStart,
                                    data.begin() + nalEnd);
                if (nalType == 7) {
                    h264Sps_ = std::move(parameterSet);
                    // SPS: [NAL header, profile_idc, profile_compatibility, level_idc, ...]
                    if (nalStart + 3 < nalEnd) {
                        h264CodecProfile_ = (static_cast<uint32_t>(data[nalStart + 1]) << 16) |
                            (static_cast<uint32_t>(data[nalStart + 2]) << 8) |
                            static_cast<uint32_t>(data[nalStart + 3]);
                    }
                } else {
                    h264Pps_ = std::move(parameterSet);
                }
            }
        }
        if (nextStart >= data.size()) {
            break;
        }
        start = nextStart;
    }
}

void EncoderMf::PrependCachedParameterSets(EncodedChunk& chunk) const {
    if (!chunk.isKey) {
        return;
    }
    const bool hasSps = HasH264NalType(chunk.data, 7);
    const bool hasPps = HasH264NalType(chunk.data, 8);
    if ((hasSps || h264Sps_.empty()) && (hasPps || h264Pps_.empty())) {
        return;
    }

    std::vector<uint8_t> completeAccessUnit;
    completeAccessUnit.reserve(chunk.data.size() +
        (hasSps ? 0 : h264Sps_.size()) + (hasPps ? 0 : h264Pps_.size()));
    if (!hasSps) {
        completeAccessUnit.insert(completeAccessUnit.end(), h264Sps_.begin(), h264Sps_.end());
    }
    if (!hasPps) {
        completeAccessUnit.insert(completeAccessUnit.end(), h264Pps_.begin(), h264Pps_.end());
    }
    completeAccessUnit.insert(completeAccessUnit.end(), chunk.data.begin(), chunk.data.end());
    chunk.data = std::move(completeAccessUnit);
}

void EncoderMf::CoalesceH264AccessUnits(std::vector<EncodedChunk>& chunks) {
    if (chunks.size() < 2) {
        return;
    }
    std::vector<EncodedChunk> accessUnits;
    accessUnits.reserve(chunks.size());
    for (auto& chunk : chunks) {
        // 同一时间戳的多个 MFT output sample 属于同一个 access unit；WebCodecs
        // 必须把它们作为单个 EncodedVideoChunk 交给解码器，不能让发送队列覆盖。
        const auto existing = std::find_if(accessUnits.begin(), accessUnits.end(),
            [&chunk](const EncodedChunk& unit) {
                return unit.timestampUs == chunk.timestampUs;
            });
        if (existing == accessUnits.end()) {
            accessUnits.push_back(std::move(chunk));
            continue;
        }
        existing->data.insert(existing->data.end(), chunk.data.begin(), chunk.data.end());
        existing->isKey = existing->isKey || chunk.isKey;
    }
    chunks = std::move(accessUnits);
}

bool EncoderMf::DrainH264(std::vector<EncodedChunk>& out) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output{};
        Microsoft::WRL::ComPtr<IMFSample> callerSample;
        if ((outputStreamInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
            const DWORD outputSize = std::max<DWORD>(1, outputStreamInfo_.cbSize);
            HRESULT hr = MFCreateSample(&callerSample);
            if (FAILED(hr)) return false;
            hr = MFCreateMemoryBuffer(outputSize, &outputBuffer);
            if (FAILED(hr)) return false;
            callerSample->AddBuffer(outputBuffer.Get());
            output.pSample = callerSample.Get();
        }

        DWORD status = 0;
        const HRESULT hr = h264Mft_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) {
            output.pEvents->Release();
            output.pEvents = nullptr;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!ConfigureH264OutputType()) {
                log::Error("H.264 output type renegotiation failed");
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            log::Error("H.264 ProcessOutput failed hr=" + std::to_string(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<IMFSample> outputSample;
        if (output.pSample == callerSample.Get()) {
            outputSample = callerSample;
        } else if (output.pSample) {
            outputSample.Attach(output.pSample);
        } else {
            log::Error("H.264 ProcessOutput returned no sample");
            return false;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
        if (FAILED(outputSample->ConvertToContiguousBuffer(&outputBuffer))) {
            return false;
        }
        BYTE* bytes = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        if (FAILED(outputBuffer->Lock(&bytes, &maxLength, &currentLength))) {
            return false;
        }
        EncodedChunk chunk;
        const bool normalized = NormalizeH264ToAnnexB(bytes, currentLength, chunk.data);
        outputBuffer->Unlock();
        if (!normalized) {
            log::Warn("H.264 MFT returned an unsupported NAL layout");
            return false;
        }

        CacheH264ParameterSets(chunk.data);
        // EncodedVideoChunk 的 key 必须确实包含 IDR。仅依据 MFT 的 CleanPoint
        // 属性会把单独输出的 SPS/PPS 误标为 key，导致浏览器解码状态损坏。
        chunk.isKey = HasH264NalType(chunk.data, 5);
        if (chunk.isKey) {
            PrependCachedParameterSets(chunk);
            keyFrameRequested_ = false;
        }
        LONGLONG timestamp100Ns = static_cast<LONGLONG>(frameIndex_) *
            kHundredNsPerSecond / std::max(1, fps_);
        outputSample->GetSampleTime(&timestamp100Ns);
        chunk.timestampUs = timestamp100Ns < 0 ? 0 :
            static_cast<uint64_t>(timestamp100Ns) / 10;
        out.push_back(std::move(chunk));
    }
}

bool EncoderMf::EncodeH264(const uint8_t* bgra, std::vector<EncodedChunk>& out) {
    out.clear();
    const size_t nv12Bytes = static_cast<size_t>(width_) * height_ * 3 / 2;
    if (nv12Bytes > std::numeric_limits<DWORD>::max()) {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> inputBuffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12Bytes), &inputBuffer);
    if (FAILED(hr)) return false;
    BYTE* nv12 = nullptr;
    DWORD maxLength = 0;
    if (FAILED(inputBuffer->Lock(&nv12, &maxLength, nullptr))) return false;
    ConvertBgraToNv12(bgra, nv12);
    inputBuffer->Unlock();
    inputBuffer->SetCurrentLength(static_cast<DWORD>(nv12Bytes));

    Microsoft::WRL::ComPtr<IMFSample> inputSample;
    hr = MFCreateSample(&inputSample);
    if (FAILED(hr)) return false;
    inputSample->AddBuffer(inputBuffer.Get());
    const LONGLONG timestamp100Ns = static_cast<LONGLONG>(frameIndex_) *
        kHundredNsPerSecond / std::max(1, fps_);
    inputSample->SetSampleTime(timestamp100Ns);
    inputSample->SetSampleDuration(kHundredNsPerSecond / std::max(1, fps_));
    const uint64_t keyFrameInterval = static_cast<uint64_t>(std::max(1, fps_)) *
        kKeyFrameIntervalSeconds;
    if (keyFrameRequested_ || frameIndex_ % keyFrameInterval == 0) {
        ForceH264KeyFrame();
        inputSample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    hr = h264Mft_->ProcessInput(0, inputSample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!DrainH264(out)) {
            return false;
        }
        hr = h264Mft_->ProcessInput(0, inputSample.Get(), 0);
    }
    if (FAILED(hr)) {
        log::Error("H.264 ProcessInput failed hr=" + std::to_string(hr));
        return false;
    }
    ++frameIndex_;
    if (!DrainH264(out)) {
        return false;
    }
    CoalesceH264AccessUnits(out);
    return true;
}

bool EncoderMf::InitJpeg() {
    quality_ = 55;  // 低延迟远控:固定较低质量,减小帧体积。
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&wicFactory_));
    if (FAILED(hr)) {
        log::Error("WIC factory failed hr=" + std::to_string(hr));
        return false;
    }
    mode_ = EncoderMode::kJpeg;
    configured_ = true;
    log::Info("JPEG encoder ready: " + std::to_string(width_) + "x" +
              std::to_string(height_) + " q=" + std::to_string(quality_));
    return true;
}

bool EncoderMf::EncodeJpeg(const uint8_t* bgra, std::vector<EncodedChunk>& out) {
    auto* stream = new MemoryStream();
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    HRESULT hr = wicFactory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) { stream->Release(); return false; }
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { stream->Release(); return false; }

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    hr = wicFactory_->CreateBitmapFromMemory(width_, height_, GUID_WICPixelFormat32bppBGRA,
        width_ * 4, width_ * height_ * 4, const_cast<BYTE*>(bgra), &bitmap);
    if (FAILED(hr)) { stream->Release(); log::Error("WIC bitmap failed hr=" + std::to_string(hr)); return false; }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) { stream->Release(); return false; }
    if (props) {
        PROPBAG2 bag{};
        WCHAR qualityName[] = L"ImageQuality";
        bag.pstrName = qualityName;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<FLOAT>(quality_) / 100.0f;
        props->Write(1, &bag, &value);
        VariantClear(&value);
    }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) { stream->Release(); return false; }
    hr = frame->WriteSource(bitmap.Get(), nullptr);
    if (FAILED(hr)) { stream->Release(); log::Error("WIC write failed hr=" + std::to_string(hr)); return false; }
    hr = frame->Commit();
    if (FAILED(hr)) {
        stream->Release();
        log::Error("WIC frame commit failed hr=" + std::to_string(hr));
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        stream->Release();
        log::Error("WIC encoder commit failed hr=" + std::to_string(hr));
        return false;
    }

    EncodedChunk chunk;
    chunk.data = stream->TakeData();
    stream->Release();
    if (chunk.data.empty()) {
        return false;
    }
    chunk.isKey = true;
    chunk.timestampUs = frameIndex_ * kMicrosecondsPerSecond / std::max(1, fps_);
    ++frameIndex_;
    out.clear();
    out.push_back(std::move(chunk));
    return true;
}

bool EncoderMf::Encode(const uint8_t* bgra, std::vector<EncodedChunk>& out) {
    if (!configured_ || !bgra) {
        return false;
    }
    if (mode_ != EncoderMode::kH264) {
        return EncodeJpeg(bgra, out);
    }
    if (EncodeH264(bgra, out)) {
        return true;
    }

    // 驱动重置、切换桌面或显卡休眠后硬件 MFT 可能在运行期失效。立即切回
    // JPEG，后续由 Agent 下发新 cfg，避免远控页面长时间黑屏等待手工重启。
    log::Warn("H.264 encoder failed at runtime, switching to JPEG fallback");
    ReleaseH264();
    configured_ = false;
    mode_ = EncoderMode::kJpeg;
    if (!InitJpeg()) {
        configured_ = false;
        return false;
    }
    return EncodeJpeg(bgra, out);
}

void EncoderMf::ReleaseH264() {
    if (h264Mft_.Get()) {
        h264Mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        h264Mft_.Reset();
    }
    codecApi_.Reset();
    outputStreamInfo_ = {};
    hardwareH264_ = false;
    h264Sps_.clear();
    h264Pps_.clear();
    h264CodecProfile_ = 0x42E01E;
    keyFrameRequested_ = true;
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
}

void EncoderMf::Release() {
    configured_ = false;
    wicFactory_.Reset();
    ReleaseH264();
    width_ = 0;
    height_ = 0;
    frameIndex_ = 0;
    mode_ = EncoderMode::kJpeg;
}

}  // namespace remote_assist
