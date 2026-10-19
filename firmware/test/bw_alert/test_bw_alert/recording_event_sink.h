#pragma once

// Private IEventSink fakes for the bw_alert suite. Per the host-testing guidance,
// shared fakes belong to bw_testkit (ask the F00 owner / file a CR) while
// helpers only this feature needs stay in the feature's own test folder.

#include <cstddef>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace f10test {

// [EXPERIMENTAL] Why 32: a full ladder timeline in one test produces a
// handful of transitions (raise, escalate, stop); 32 leaves room for a long
// scripted sequence without letting a runaway event flood pass unnoticed -
// anything beyond it lands in dropped(), never silently (CR-004).
constexpr size_t kEventSinkCapacity = 32;

class RecordingEventSink final : public bw::IEventSink {
public:
    bw::Result<void> publish(const bw::Event& event) override {
        // Fault injection (host-testing item 6): a refusing sink writes
        // nothing at all - CR-004's Full sense - and the caller sees why.
        if (forced_err_ != bw::Err::Ok) {
            return bw::Result<void>{forced_err_};
        }
        if (count_ < kEventSinkCapacity) {
            events_[count_++] = event;
        } else {
            ++dropped_;  // entries lost, but counted - never a crash
        }
        return bw::Result<void>{bw::Err::Ok};
    }

    void forceError(bw::Err err) { forced_err_ = err; }

    size_t count() const { return count_; }
    size_t dropped() const { return dropped_; }
    const bw::Event& at(size_t i) const { return events_[i]; }
    // Precondition: count() > 0.
    const bw::Event& last() const { return events_[count_ - 1]; }
    void clear() {
        count_ = 0;
        dropped_ = 0;
    }

private:
    bw::Event events_[kEventSinkCapacity]{};
    size_t count_ = 0;
    size_t dropped_ = 0;
    bw::Err forced_err_ = bw::Err::Ok;
};

}  // namespace f10test
