#pragma once

#include "common/Config.h"

#include <array>
#include <cstdint>

namespace remote_assist {

// 输出档位按索引从高到低排列。Agent 的自适应逻辑可在压力下向更低档位移动，
// 但恢复时不会越过由 quality_cap 计算出的最高质量档位。
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

// 将用户配置转换为可恢复到的最高质量档位。调用方只会传入已通过 Config 校验的
// 数值；对意外值仍保守返回最低档，避免无效配置导致输出超过预期上限。
constexpr int ResolutionTierForQualityCap(int qualityCap) {
    if (!IsQualityCapValid(qualityCap)) {
        return static_cast<int>(kStreamResolutionCaps.size()) - 1;
    }
    if (qualityCap == static_cast<int>(QualityCap::kAutomatic)) {
        return 0;
    }
    for (size_t tier = 0; tier < kStreamResolutionCaps.size(); ++tier) {
        if (kStreamResolutionCaps[tier].height <= qualityCap) {
            return static_cast<int>(tier);
        }
    }
    return static_cast<int>(kStreamResolutionCaps.size()) - 1;
}

// H.264 发送窗口满一次可能只是浏览器单次 rAF/GC 抖动；连续两个目标帧周期
// 都拿不到编码信用才视为持续背压，避免正常局域网被一次偶发等待过早降帧。
constexpr bool HasSustainedH264CreditBackpressure(uint64_t creditWaits) {
    return creditWaits >= 2;
}

}  // namespace remote_assist
