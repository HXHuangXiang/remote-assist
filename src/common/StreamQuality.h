#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace remote_assist {

// 输出档位按索引从高到低排列。自动模式可在压力下向更低档位移动；固定模式
// 会锁定所选档位，仍允许 Agent 自适应帧率和码率以保证远程操作可用。
struct StreamResolutionCap {
    int width;
    int height;
};

inline constexpr std::array<StreamResolutionCap, 5> kStreamResolutionCaps = {{
    {1920, 1080},
    {1600, 900},
    {1280, 720},
    {960, 540},
    {640, 360},
}};

// 网页传入的推流画质模式。original 保持被控端当前采集尺寸，其余固定档位只
// 限制分辨率；automatic 才允许 Agent 在链路压力下切换分辨率档位。
enum class StreamQuality : int {
    kAutomatic = 0,
    kOriginal = 1,
    k1080p = 2,
    k720p = 3,
    k540p = 4,
    k360p = 5,
};

constexpr int kDefaultPatchThresholdPercent = 50;
constexpr int kMinPatchThresholdPercent = 10;
constexpr int kMaxPatchThresholdPercent = 90;
// 网页控制端可以在固定模式下选择的帧率与码率范围。码率统一使用 bit/s，网页负责
// 将显示的 MB/s 转换后再传输，避免协议端出现浮点误差。
constexpr int kMinStreamFps = 1;
constexpr int kMaxStreamFps = 60;
constexpr int kMinStreamBitrate = 100'000;
constexpr int kMaxStreamBitrate = 50'000'000;

constexpr bool IsStreamQualityValid(int quality) {
    return quality >= static_cast<int>(StreamQuality::kAutomatic) &&
        quality <= static_cast<int>(StreamQuality::k360p);
}

constexpr bool IsPatchThresholdValid(int percent) {
    return percent >= kMinPatchThresholdPercent &&
        percent <= kMaxPatchThresholdPercent && percent % 5 == 0;
}

constexpr bool IsStreamFpsValid(int fps) {
    return fps >= kMinStreamFps && fps <= kMaxStreamFps;
}

constexpr bool IsStreamBitrateValid(int bitrate) {
    return bitrate >= kMinStreamBitrate && bitrate <= kMaxStreamBitrate;
}

constexpr bool IsFixedStreamQuality(StreamQuality quality) {
    return quality != StreamQuality::kAutomatic;
}

constexpr int ResolutionTierForStreamQuality(StreamQuality quality) {
    switch (quality) {
    case StreamQuality::kAutomatic:
    case StreamQuality::kOriginal:
    case StreamQuality::k1080p:
        return 0;
    case StreamQuality::k720p:
        return 2;
    case StreamQuality::k540p:
        return 3;
    case StreamQuality::k360p:
        return 4;
    }
    return static_cast<int>(kStreamResolutionCaps.size()) - 1;
}

inline const char* StreamQualityName(StreamQuality quality) {
    switch (quality) {
    case StreamQuality::kAutomatic: return "auto";
    case StreamQuality::kOriginal: return "original";
    case StreamQuality::k1080p: return "1080p";
    case StreamQuality::k720p: return "720p";
    case StreamQuality::k540p: return "540p";
    case StreamQuality::k360p: return "360p";
    }
    return "auto";
}

// H.264 发送窗口满一次可能只是浏览器单次 rAF/GC 抖动；连续两个目标帧周期
// 都拿不到编码信用才视为持续背压，避免正常局域网被一次偶发等待过早降帧。
constexpr bool HasSustainedH264CreditBackpressure(uint64_t creditWaits) {
    return creditWaits >= 2;
}

}  // namespace remote_assist
