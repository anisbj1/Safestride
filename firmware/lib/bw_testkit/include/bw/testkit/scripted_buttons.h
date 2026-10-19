#pragma once

#include <bw/testkit/host_only.h>

#include <bw/clock.h>
#include <bw/input.h>
#include <bw/result.h>
#include <bw/types.h>

namespace bw::testkit {

// [EXPERIMENTAL] Script capacity. Why 32: a button test script is a handful of
// transitions per scenario (press, release, long press, double press), so 32
// holds 10+ scenarios in one fixture; a full script is refused with Err::Full
// (CR-004: nothing written) rather than silently truncated. Raise if a replay
// test needs more.
constexpr size_t kDefaultButtonScriptEntries = 32;

// ScriptedButtons — bw::IButtonInput fake (02 §3).
//
// Two modes, both deterministic:
//  - immediate: setPressed() flips a button's state right away (no clock needed);
//  - scripted: pressAt() queues (t_ms, button, pressed) transitions that are
//    applied the first time isPressedRaw() observes the clock at or after their
//
// isPressedRaw() is const by contract, so the script cursor and the pressed
// states are `mutable`: observing input advances the replay. That is deliberate
// and documented here rather than hidden.
class ScriptedButtons final : public bw::IButtonInput {
public:
    // The clock drives the scripted mode; may be null (immediate mode only).
    explicit ScriptedButtons(const bw::IClock* clock = nullptr) : clock_(clock) {}

    bool isPressedRaw(bw::ButtonId id) const override {
        applyDueScript();
        return pressed_[indexOf(id)];
    }

    // Immediate mode: applies now, independent of any queued script entry.
    void setPressed(bw::ButtonId id, bool pressed) { pressed_[indexOf(id)] = pressed; }

    // Queues a transition for the scripted mode. Entries are observed in time
    // order only if the test enqueues them that way (no sorting, no heap).
    bw::Result<void> pressAt(bw::TimeMs t_ms, bw::ButtonId id, bool pressed) {
        if (script_count_ >= kDefaultButtonScriptEntries) {
            return {bw::Err::Full};  // CR-004: refused, nothing written
        }
        script_[script_count_] = ScriptEntry{t_ms, id, pressed};
        ++script_count_;
        return {bw::Err::Ok};
    }

    // Script entries not yet observed by isPressedRaw().
    size_t pendingScript() const { return script_count_ - script_cursor_; }

    // Clears the script, the cursor and both pressed states.
    void reset() {
        script_count_ = 0;
        script_cursor_ = 0;
        pressed_[0] = false;
        pressed_[1] = false;
    }

private:
    struct ScriptEntry {
        bw::TimeMs t_ms;
        bw::ButtonId button;
        bool pressed;
    };

    static size_t indexOf(bw::ButtonId id) { return id == bw::ButtonId::Big ? 0 : 1; }

    // Applies every queued transition whose time has come. Clock-less setups never
    // advance the script (documented above), so immediate mode is unaffected.
    void applyDueScript() const {
        if (clock_ == nullptr) {
            return;
        }
        const bw::TimeMs now = clock_->nowMs();
        while (script_cursor_ < script_count_ && script_[script_cursor_].t_ms <= now) {
            const ScriptEntry& entry = script_[script_cursor_];
            pressed_[indexOf(entry.button)] = entry.pressed;
            ++script_cursor_;
        }
    }

    const bw::IClock* clock_;  // not owned; FakeClock lives in the test, not here
    ScriptEntry script_[kDefaultButtonScriptEntries];
    size_t script_count_ = 0;
    mutable size_t script_cursor_ = 0;
    mutable bool pressed_[2] = {false, false};
};

}  // namespace bw::testkit
