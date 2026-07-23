#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace remote_assist {

// 视频窗口包含“待发送 + 已写入但未确认”的总数。两个槽位允许网络写、WebCodecs
// 解码和下一帧传输重叠，同时仍把可见延迟约束在很小范围内。
inline constexpr size_t kFrameWindowCapacity = 2;
inline constexpr uint32_t kH264AckTimeoutMinMs = 350;
inline constexpr uint32_t kH264AckTimeoutInitialMs = 600;
inline constexpr uint32_t kH264AckTimeoutMaxMs = 1000;

// H.264 delta 不能覆盖尚未发送的前序帧，也不能在一次超时恢复已经开始后继续
// 写入旧 GOP。关键帧可取代待发送旧帧，作为新的独立解码起点。
constexpr bool H264DeltaRequiresResync(bool h264, bool isKeyFrame, bool hasPendingFrame,
                                       bool resyncAlreadyRequested) {
    return h264 && !isKeyFrame && (hasPendingFrame || resyncAlreadyRequested);
}

// 采集线程在编码下一张 H.264 delta 前检查端到端窗口。待发送帧也必须计入，
// 否则慢客户端会比配置窗口额外积压一张过时画面。
constexpr bool HasH264FrameCredit(bool hasPendingFrame, size_t inFlightFrames,
                                  bool resyncRequested, bool stopping) {
    return !stopping && !resyncRequested && !hasPendingFrame &&
        inFlightFrames < kFrameWindowCapacity;
}

// ACK 时用真实“写入完成 -> 浏览器呈现”时间调整 H.264 确认窗口。首个样本直接
// 收敛，后续使用 3/4 的旧窗口平滑一次 GC/rAF 抖动。窗口始终受 350~1000ms 限制。
constexpr uint32_t NextH264AckTimeoutAfterAck(uint32_t previousWindowMs, bool initialized,
                                               uint64_t observedMs) {
    const uint64_t doubledObserved = observedMs >
            (std::numeric_limits<uint64_t>::max() - 75) / 2
        ? std::numeric_limits<uint64_t>::max()
        : observedMs * 2 + 75;
    const uint64_t desired = doubledObserved < kH264AckTimeoutMinMs
        ? kH264AckTimeoutMinMs
        : (doubledObserved > kH264AckTimeoutMaxMs ? kH264AckTimeoutMaxMs : doubledObserved);
    if (!initialized) {
        return static_cast<uint32_t>(desired);
    }
    const uint64_t smoothed = (static_cast<uint64_t>(previousWindowMs) * 3 + desired + 3) / 4;
    return static_cast<uint32_t>(smoothed < kH264AckTimeoutMinMs
        ? kH264AckTimeoutMinMs
        : (smoothed > kH264AckTimeoutMaxMs ? kH264AckTimeoutMaxMs : smoothed));
}

// ACK 超时意味着浏览器没有在实时窗口内呈现旧 GOP。逐次放宽下一次窗口可避免
// 首个慢帧尚未返回 ACK 时反复重建 GOP；成功 ACK 后会由上面的函数重新收敛。
constexpr uint32_t NextH264AckTimeoutAfterTimeout(uint32_t previousWindowMs) {
    const uint64_t widened = static_cast<uint64_t>(previousWindowMs) * 5 / 4 + 50;
    return static_cast<uint32_t>(widened > kH264AckTimeoutMaxMs
        ? kH264AckTimeoutMaxMs
        : (widened < kH264AckTimeoutInitialMs ? kH264AckTimeoutInitialMs : widened));
}

}  // namespace remote_assist
