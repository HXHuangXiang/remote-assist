#include "net/ControllerHandoff.h"
#include "common/StreamQuality.h"
#include "net/H264FlowControl.h"

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

bool Expect(bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::cerr << "FAILED: " << message << '\n';
    return false;
}

}  // namespace

int main() {
    using remote_assist::IsPatchThresholdValid;
    using remote_assist::IsFixedStreamQuality;
    using remote_assist::IsStreamQualityValid;
    using remote_assist::H264DeltaRequiresResync;
    using remote_assist::HasSustainedH264CreditBackpressure;
    using remote_assist::HasH264FrameCredit;
    using remote_assist::ControllerHandoff;
    using remote_assist::NextH264AckTimeoutAfterAck;
    using remote_assist::NextH264AckTimeoutAfterTimeout;
    using remote_assist::ResolutionTierForStreamQuality;
    using remote_assist::StreamQuality;
    using remote_assist::kStreamResolutionCaps;

    bool ok = true;
    ok &= Expect(IsStreamQualityValid(static_cast<int>(StreamQuality::kAutomatic)),
                 "automatic quality should be valid");
    ok &= Expect(IsStreamQualityValid(static_cast<int>(StreamQuality::kOriginal)),
                 "original quality should be valid");
    ok &= Expect(IsStreamQualityValid(static_cast<int>(StreamQuality::k360p)),
                 "360p quality should be valid");
    ok &= Expect(!IsStreamQualityValid(99), "unsupported stream quality should be rejected");
    ok &= Expect(ResolutionTierForStreamQuality(StreamQuality::kAutomatic) == 0,
                 "automatic should begin at 1080p tier");
    ok &= Expect(ResolutionTierForStreamQuality(StreamQuality::kOriginal) == 0,
                 "original should use the top tier before removing its output cap");
    ok &= Expect(ResolutionTierForStreamQuality(StreamQuality::k1080p) == 0,
                 "1080p should select the 1080p tier");
    ok &= Expect(ResolutionTierForStreamQuality(StreamQuality::k720p) == 2,
                 "720p should skip 1080p and 900p tiers");
    ok &= Expect(ResolutionTierForStreamQuality(StreamQuality::k540p) == 3,
                 "540p should select the 540p tier");
    ok &= Expect(ResolutionTierForStreamQuality(StreamQuality::k360p) == 4,
                 "360p should select the lowest tier");
    ok &= Expect(!IsFixedStreamQuality(StreamQuality::kAutomatic) &&
                     IsFixedStreamQuality(StreamQuality::kOriginal) &&
                     IsFixedStreamQuality(StreamQuality::k1080p) &&
                     IsFixedStreamQuality(StreamQuality::k360p),
                 "only automatic quality may adapt its output resolution");
    ok &= Expect(kStreamResolutionCaps[ResolutionTierForStreamQuality(
                     StreamQuality::k720p)].height == 720,
                 "720p quality should select 720p height");
    ok &= Expect(IsPatchThresholdValid(10) && IsPatchThresholdValid(50) &&
                     IsPatchThresholdValid(90),
                 "patch thresholds at supported bounds should be valid");
    ok &= Expect(!IsPatchThresholdValid(9) && !IsPatchThresholdValid(91) &&
                     !IsPatchThresholdValid(52),
                 "patch threshold must use the supported five-percent steps");

    ok &= Expect(!H264DeltaRequiresResync(true, true, true, true),
                 "H.264 key frames may replace an obsolete pending frame");
    ok &= Expect(H264DeltaRequiresResync(true, false, true, false),
                 "H.264 delta cannot replace a pending frame");
    ok &= Expect(H264DeltaRequiresResync(true, false, false, true),
                 "H.264 delta must wait for an IDR after resync request");
    ok &= Expect(!H264DeltaRequiresResync(false, false, true, true),
                 "JPEG frames do not have a GOP dependency");

    ok &= Expect(HasH264FrameCredit(false, 0, false, false),
                 "an empty H.264 window should have credit");
    ok &= Expect(HasH264FrameCredit(false, 1, false, false),
                 "one in-flight H.264 frame should retain one credit");
    ok &= Expect(!HasH264FrameCredit(false, 2, false, false),
                 "two in-flight H.264 frames should fill the window");
    ok &= Expect(!HasH264FrameCredit(true, 0, false, false),
                 "a pending H.264 frame should consume a window slot");
    ok &= Expect(!HasH264FrameCredit(false, 0, true, false),
                 "resync request should block H.264 delta encoding");
    ok &= Expect(!HasH264FrameCredit(false, 0, false, true),
                 "stopping broadcaster should not grant H.264 encoding credit");
    ok &= Expect(!HasH264FrameCredit(true, 1, true, false),
                 "resync state must reject H.264 credit even below the frame window cap");
    ok &= Expect(!HasSustainedH264CreditBackpressure(0) &&
                     !HasSustainedH264CreditBackpressure(1),
                 "a single H.264 credit wait should not trigger adaptive downshift");
    ok &= Expect(HasSustainedH264CreditBackpressure(2),
                 "repeated H.264 credit waits should trigger adaptive downshift");

    ControllerHandoff handoff;
    ok &= Expect(handoff.CanAccept() && handoff.TryAttach(),
                 "an idle handoff should accept its first controller");
    ok &= Expect(handoff.HasActiveController() && !handoff.CanAccept(),
                 "an attached controller should own the admission slot");
    handoff.BeginInputCleanup();
    ok &= Expect(handoff.IsInputCleanupPending() && !handoff.HasActiveController() &&
                     !handoff.CanAccept(),
                 "input cleanup should stop streaming and block a replacement controller");
    handoff.Detach();
    ok &= Expect(!handoff.CanAccept(),
                 "detaching before key release must keep controller admission blocked");
    handoff.CompleteInputCleanup();
    ok &= Expect(!handoff.IsInputCleanupPending() && handoff.CanAccept(),
                 "controller admission should reopen only after the input release completes");
    ok &= Expect(handoff.TryAttach(), "a completed handoff should accept a new controller");
    handoff.BeginInputCleanup();
    handoff.CompleteInputCleanup();
    ok &= Expect(handoff.IsInputCleanupPending() && !handoff.CanAccept(),
                 "handler teardown after input cleanup must remain a retryable handoff state");
    handoff.Detach();
    ok &= Expect(!handoff.IsInputCleanupPending() && handoff.CanAccept(),
                 "handler teardown should finish an already released controller handoff");

    ok &= Expect(NextH264AckTimeoutAfterAck(600, false, 0) == 350,
                 "first low-latency ACK should clamp to the minimum timeout");
    ok &= Expect(NextH264AckTimeoutAfterAck(600, false, 200) == 475,
                 "first ACK should derive timeout from observed presentation latency");
    ok &= Expect(NextH264AckTimeoutAfterAck(475, true, 100) == 444,
                 "subsequent ACK should smooth timeout changes");
    ok &= Expect(NextH264AckTimeoutAfterAck(600, true, 800) == 700,
                 "high ACK latency should be smoothed before reaching the cap");
    ok &= Expect(NextH264AckTimeoutAfterTimeout(600) == 800,
                 "first timeout should widen the H.264 ACK window");
    ok &= Expect(NextH264AckTimeoutAfterTimeout(800) == 1000,
                 "timeout widening should be capped");
    ok &= Expect(NextH264AckTimeoutAfterTimeout(1000) == 1000,
                 "an already capped timeout should remain capped");
    ok &= Expect(NextH264AckTimeoutAfterAck(
                     1000, true, std::numeric_limits<uint64_t>::max()) == 1000,
                 "extreme ACK latency must saturate without integer overflow");
    return ok ? 0 : 1;
}
