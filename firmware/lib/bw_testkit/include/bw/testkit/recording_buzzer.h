#pragma once

#include <bw/testkit/host_only.h>

#include <bw/clock.h>
#include <bw/hardware.h>
#include <bw/types.h>

namespace bw::testkit {

// [EXPERIMENTAL] Recording capacity. Why 64: a wake-alert pattern (F10, loudest
// pattern approved in 04 §3 item 7) is a few dozen on/off transitions; overflow is
// counted in dropped(), never silent (CR-004).
constexpr size_t kDefaultBuzzerRecordEntries = 64;

// RecordingBuzzer — bw::IBuzzer fake (02 §3): remembers every set() call in order
// so a test can assert the exact on/off + frequency pattern and its timing.
// Clock required for the same reason as RecordingLed: a recorded t_ms must be real
// monotonic time, never a fabricated 0 (02 §1).
template <size_t Capacity = kDefaultBuzzerRecordEntries>
class RecordingBuzzer final : public bw::IBuzzer {
public:
    struct Entry {
        bw::TimeMs t_ms;
        bool on;
        uint16_t freq_hz;
    };

    explicit RecordingBuzzer(const bw::IClock& clock) : clock_(&clock) {}

    void set(bool on, uint16_t freq_hz) override {
        if (count_ < Capacity) {
            entries_[count_] = Entry{clock_->nowMs(), on, freq_hz};
            ++count_;
        } else {
            ++dropped_;  // CR-004 Overflow sense: entries lost, but counted, never a crash
        }
    }

    // Precondition: i < count().
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
