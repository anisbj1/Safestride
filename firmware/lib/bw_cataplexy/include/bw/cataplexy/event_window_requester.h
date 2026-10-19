#pragma once

// bw/cataplexy/event_window_requester.h — F09 §7 EventWindowRequester, spec §1/§3.
//
// Phase 1 of the capture-first delivery (spec §6 D1): the user marks an event,
// the requester holds the marker and, once kPostEventCaptureMs has elapsed,
// reports the exact span to persist — [mark - kPreEventCaptureMs,
// mark + kPostEventCaptureMs]. The raw ring buffer belongs to F18 (spec §11),
// so this class only computes the request; nothing is written here.
//
// Research-mode gate (spec §5 must-not): a window is only handed out while
// F24's OperatingMode is Research — the capture path never runs in Day mode.
//
// Pure logic: TimeMs comes in as an argument, no platform calls (01 §1 rule 5).

#include <cstddef>
#include <cstdint>

#include <bw/cataplexy/cataplexy_config.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::cataplexy {

// The span F18 should persist around one marker. t_start_ms is clamped at 0:
// TimeMs 0 is boot (02 §1), so a marker inside the first kPreEventCaptureMs
// has no earlier context to capture.
struct CaptureWindow {
    TimeMs mark_ms = 0;
    TimeMs t_start_ms = 0;
    TimeMs t_end_ms = 0;
    ReasonMask reasons = 0;  // ReasonCode::UserMarked — why this span exists
};

class EventWindowRequester {
public:
    // Registers a marker (wiring feeds Command{MarkEvent} here).
    // Returns false when a capture is already in flight (kMaxPendingCaptures
    // == 1: a second mark inside the 60 s span describes the same episode, so
    // the first marker wins rather than the request being half-overwritten).
    bool onMark(TimeMs mark_ms);

    // One tick. Returns true and fills `out` when the post-window has elapsed:
    //   mode == Research  -> the window is emitted (spec §5 must-not #3)
    //   mode != Research  -> the pending capture is dropped, nothing is emitted
    bool step(TimeMs now_ms, OperatingMode mode, CaptureWindow& out);

    bool pending() const { return pending_; }
    TimeMs markMs() const { return mark_ms_; }

    // Drops any pending capture (used when the mode leaves Research before the
    // post-window elapsed, and by tests).
    void reset();

private:
    TimeMs mark_ms_ = 0;
    bool pending_ = false;
};

}  // namespace bw::cataplexy
