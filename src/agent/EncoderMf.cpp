#include "agent/EncoderMf.h"
#include "common/Log.h"
#include <cstring>
#pragma comment(lib, "windowscodecs.lib")

namespace remote_assist {

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
        ULONG avail = (ULONG)(buf_.size() - pos_); ULONG n = (cb < avail) ? cb : avail;
        if (n) memcpy(pv, buf_.data() + pos_, n); pos_ += n;
        if (pcbRead) *pcbRead = n; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override {
        if (pos_ + cb > buf_.size()) buf_.resize(pos_ + cb);
        memcpy(buf_.data() + pos_, pv, cb); pos_ += cb;
        if (pcbWritten) *pcbWritten = cb; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER d, DWORD origin, ULARGE_INTEGER* p) override {
        int64_t np = pos_;
        if (origin == STREAM_SEEK_SET) np = d.QuadPart;
        else if (origin == STREAM_SEEK_CUR) np += d.QuadPart;
        else np = (int64_t)buf_.size() + d.QuadPart;
        if (np < 0) return STG_E_INVALIDFUNCTION;
        pos_ = (size_t)np; if (p) p->QuadPart = pos_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER s) override { buf_.resize((size_t)s.QuadPart); return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* p, DWORD) override { if (p) { memset(p, 0, sizeof(*p)); p->cbSize.QuadPart = buf_.size(); } return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }
    const std::vector<uint8_t>& Data() const { return buf_; }
private:
    LONG ref_; size_t pos_; std::vector<uint8_t> buf_;
};

EncoderMf::~EncoderMf() { Release(); }

bool EncoderMf::Init(int width, int height, int fps, int bitrateBps) {
    width_ = width; height_ = height;
    quality_ = 50 + (bitrateBps / 100000);
    if (quality_ > 95) quality_ = 95;
    if (quality_ < 40) quality_ = 40;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory_));
    if (FAILED(hr)) { log::Error("WIC factory failed hr=" + std::to_string(hr)); return false; }
    configured_ = true;
    log::Info("JPEG encoder ready: " + std::to_string(width) + "x" + std::to_string(height) + " q=" + std::to_string(quality_));
    return true;
}

bool EncoderMf::Encode(const uint8_t* bgra, std::vector<EncodedChunk>& out) {
    if (!configured_ || !wicFactory_) return false;

    auto* stream = new MemoryStream();
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> enc;
    HRESULT hr = wicFactory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (FAILED(hr)) { stream->Release(); return false; }
    hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { stream->Release(); return false; }

    Microsoft::WRL::ComPtr<IWICBitmap> bmp;
    hr = wicFactory_->CreateBitmapFromMemory(width_, height_, GUID_WICPixelFormat32bppBGRA,
        width_ * 4, width_ * height_ * 4, const_cast<BYTE*>(bgra), &bmp);
    if (FAILED(hr)) { stream->Release(); log::Error("WIC bitmap failed hr=" + std::to_string(hr)); return false; }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> props;
    hr = enc->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) { stream->Release(); return false; }

    // 设 JPEG 质量。
    if (props) {
        PROPBAG2 bag = {}; bag.pstrName = L"ImageQuality";
        VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = (FLOAT)quality_ / 100.0f;
        props->Write(1, &bag, &v); VariantClear(&v);
    }
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) { stream->Release(); return false; }
    hr = frame->WriteSource(bmp.Get(), nullptr);
    if (FAILED(hr)) { stream->Release(); log::Error("WIC write failed hr=" + std::to_string(hr)); return false; }
    frame->Commit();
    enc->Commit();

    EncodedChunk chunk;
    chunk.data = stream->Data();
    chunk.isKey = true;
    out.push_back(std::move(chunk));
    stream->Release();
    return true;
}

void EncoderMf::Release() { configured_ = false; wicFactory_.Reset(); }

}  // namespace remote_assist
