// bw/cataplexy/event_window_requester.cpp — F09 §7 EventWindowRequester (spec §1/§3).

#include <bw/cataplexy/event_window_requester.h>

namespace bw::cataplexy {

bool EventWindowRequester::onMark(TimeMs mark_ms) {
    if (pending_) {
        return false;  // one capture in flight (kMaxPendingCaptures)
    }
    mark_ms_ = mark_ms;
    pending_ = true;
    return true;
}

bool EventWindowRequester::step(TimeMs now_ms, OperatingMode mode, CaptureWindow& out) {
    static_assert(kMaxPendingCaptures == 1,
                  "this class stores one pending mark; widen it with the constant");
    if (!pending_) {
        return false;
    }

    // Not due yet (a backwards clock is "not due" too — the subtraction is
    // unsigned, so compare first): the post-window is what makes the capture
    // complete.
    if (now_ms < mark_ms_ || now_ms - mark_ms_ < kPostEventCaptureMs) {
        return false;
    }

    // The capture must not run outside Research mode (spec §5 must-not #3).
    // Checked at emit time so one place owns the gate.
    if (mode != OperatingMode::Research) {
        reset();
        return false;
    }

    out.mark_ms = mark_ms_;
    out.t_start_ms = mark_ms_ >= kPreEventCaptureMs ? mark_ms_ - kPreEventCaptureMs : 0;
    out.t_end_ms = mark_ms_ + kPostEventCaptureMs;
    out.reasons = reasonBit(ReasonCode::UserMarked);
    reset();
    return true;
}

void EventWindowRequester::reset() {
    mark_ms_ = 0;
    pending_ = false;
}

}  // namespace bw::cataplexy
