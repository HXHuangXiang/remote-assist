#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace remote_assist {

struct EncodedChunk {
    std::vector<uint8_t> data;
    bool isKey = true;
};

class EncoderMf {
public:
    EncoderMf() = default;
    ~EncoderMf();
    EncoderMf(const EncoderMf&) = delete;
    EncoderMf& operator=(const EncoderMf&) = delete;
    bool Init(int width, int height, int fps, int bitrateBps);
    bool Encode(const uint8_t* bgra, std::vector<EncodedChunk>& out);
    std::string CodecString() const { return "jpeg"; }
    void Release();
private:
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    int width_ = 0;
    int height_ = 0;
    int quality_ = 75;
    bool configured_ = false;
};

}  // namespace remote_assist
