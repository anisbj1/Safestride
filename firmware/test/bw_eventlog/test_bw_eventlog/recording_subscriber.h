#pragma once

// f17_test::RecordingSubscriber — a consumer-side bw::IEventSink fake for the
// §6 D1 fan-out tests (§15.6): records every delivered event in order, and can
// reject publishes (Err::Full) to prove one bad subscriber is counted, never
// retried, and never blocks the others (§10).

#include <cstddef>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/testkit/host_only.h>

namespace f17_test {

class RecordingSubscriber final : public bw::IEventSink {
public:
    bw::Result<void> publish(const bw::Event& event) override {
        ++calls_;
        if (reject_next_ > 0 || reject_all_) {
            reject_next_ = reject_next_ > 0 ? reject_next_ - 1 : 0;
            ++rejected_;
            return {bw::Err::Full};
        }
        if (count_ < kCapacity) {
            events_[count_] = event;
            ++count_;
            return {bw::Err::Ok};
        }
        ++overflowed_;  // test-cap overrun: counted, never silent (CR-004)
        return {bw::Err::Ok};
    }

    // Reject the next n publishes (a busy/bounded consumer), or all of them.
    void rejectNext(size_t n) { reject_next_ = n; }
    void rejectAll(bool on) { reject_all_ = on; }

    size_t calls() const { return calls_; }
    size_t rejected() const { return rejected_; }
    size_t count() const { return count_; }
    size_t overflowed() const { return overflowed_; }
    // Precondition: i < count().
    const bw::Event& at(size_t i) const { return events_[i]; }

private:
    // [EXPERIMENTAL] Why 32: a test drains at most queue-depth (16) events per
    // cycle plus Boot/TimeSync; 32 covers every scripted burst twice and keeps
    // the fake a static array (no heap, host-only).
    static constexpr size_t kCapacity = 32;

    bw::Event events_[kCapacity]{};
    size_t count_ = 0;
    size_t calls_ = 0;
    size_t rejected_ = 0;
    size_t overflowed_ = 0;
    size_t reject_next_ = 0;
    bool reject_all_ = false;
};

}  // namespace f17_test
