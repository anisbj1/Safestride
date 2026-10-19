#pragma once

#include <bw/testkit/host_only.h>

#include <bw/clock.h>
#include <bw/hardware.h>
#include <bw/types.h>

namespace bw::testkit {

// [EXPERIMENTAL] Recording capacity. Why 64: one full alert-ladder cycle (F10) is
// a few dozen LED transitions; overflowing is counted in dropped(), never silent
// (CR-004: data already lost -> the caller can see it).
constexpr size_t kDefaultLedRecordEntries = 64;

// RecordingLed — bw::IRgbLed fake (02 §3): remembers every set() call in order so
// a test can assert the exact colour sequence and when it happened.
//
// The clock is required (not optional) so every entry carries a meaningful t_ms:
// 02 §1 forbids magic values, and a fabricated t_ms = 0 would be indistinguishable
// from "set at boot". Tests that do not care about time still pass a FakeClock.
template <size_t Capacity = kDefaultLedRecordEntries>
class RecordingLed final : public bw::IRgbLed {
public:
    struct Entry {
        bw::TimeMs t_ms;
        bw::Rgb rgb;
    };

    explicit RecordingLed(const bw::IClock& clock) : clock_(&clock) {}

    void set(bw::Rgb rgb) override {
        if (count_ < Capacity) {
            entries_[count_] = Entry{clock_->nowMs(), rgb};
            ++count_;
        } else {
            ++dropped_;  // CR-004 Overflow sense: entries lost, but counted, never a crash
        }
    }

    // Precondition: i < count() — callers iterate from the test's own assertions.
    const Entry& at(size_t i) const { return entries_[i]; }
    size_t count() const { return count_; }
    size_t dropped() const { return dropped_; }

    // Precondition: count() > 0.
    const Entry& last() const { return entries_[count_ - 1]; }

    void clear() {
        count_ = 0;
        dropped_ = 0;
    }

private:
    const bw::IClock* clock_;  // not owned; FakeClock lives in the test
    Entry entries_[Capacity];
    size_t count_ = 0;
    size_t dropped_ = 0;
};

}  // namespace bw::testkit
