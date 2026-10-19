#pragma once

#include <bw/testkit/host_only.h>

#include <bw/fault.h>
#include <bw/types.h>

namespace bw::testkit {

// [EXPERIMENTAL] Recording capacity. Why 32: a fault-injection scenario reports a
// handful of faults per injection round; overflow is counted in dropped(), never
// silent (CR-004), so a test that overruns its own budget notices immediately.
constexpr size_t kDefaultFaultRecordEntries = 32;

// RecordingFaultReporter — bw::IFaultReporter fake (02 §5): remembers every
// report(code, severity, t_ms) in order. The timestamp is an argument of the
// contract itself (no clock needed): the caller passes its own monotonic time.
template <size_t Capacity = kDefaultFaultRecordEntries>
class RecordingFaultReporter final : public bw::IFaultReporter {
public:
    struct Entry {
        bw::FaultCode code;
        bw::FaultSeverity severity;
        bw::TimeMs t_ms;
    };

    void report(bw::FaultCode code, bw::FaultSeverity severity, bw::TimeMs t_ms) override {
        if (count_ < Capacity) {
            entries_[count_] = Entry{code, severity, t_ms};
            ++count_;
        } else {
            ++dropped_;  // CR-004 Overflow sense: reports lost, but counted
        }
    }

    // Precondition: i < count().
    const Entry& at(size_t i) const { return entries_[i]; }
    size_t count() const { return count_; }
    size_t dropped() const { return dropped_; }

    // How often a specific code was reported (F26-style counting in assertions).
    size_t countOf(bw::FaultCode code) const {
        size_t n = 0;
        for (size_t i = 0; i < count_; ++i) {
            n += entries_[i].code == code ? 1 : 0;
        }
        return n;
    }

    void clear() {
        count_ = 0;
        dropped_ = 0;
    }

private:
    Entry entries_[Capacity];
    size_t count_ = 0;
    size_t dropped_ = 0;
};

}  // namespace bw::testkit
