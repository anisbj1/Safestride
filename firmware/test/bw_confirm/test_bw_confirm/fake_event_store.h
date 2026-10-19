#pragma once

// firmware/test/bw_confirm/test_bw_confirm/fake_event_store.h — the fake
// IEventStore for the F14 suite (plan §7): label capture + failure injection.
// F18 is built in parallel this wave, so the suite never includes F17/F18
// headers — only the bw_core contract (plan §3).

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace bw_confirm_test {

struct LabelCall {
    uint32_t seq;
    bw::UserLabel label;
};

class FakeEventStore : public bw::IEventStore {
public:
    bw::Result<uint32_t> append(const bw::Event&) override { return {bw::Err::Ok, next_seq_++}; }

    bw::Result<size_t> readFrom(uint32_t, bw::Event*, size_t) override {
        return {bw::Err::Unsupported, size_t{0}};
    }

    bw::Result<void> setLabel(uint32_t seq, bw::UserLabel label) override {
        ++set_label_calls_;
        if (fail_next_ > 0) {
            --fail_next_;
            return {bw::Err::Full};  // refused, nothing written (CR-004 sense)
        }
        if (label_count_ < kMaxCalls) {
            calls_[label_count_] = LabelCall{seq, label};
            ++label_count_;
        }
        return {bw::Err::Ok};
    }

    bw::Result<void> markSyncedThrough(uint32_t) override { return {bw::Err::Ok}; }

    uint32_t lastSyncedSeq() const override { return 0; }

    // ---- test access --------------------------------------------------------
    int setLabelCalls() const { return set_label_calls_; }
    size_t callCount() const { return label_count_; }
    const LabelCall& callAt(size_t i) const { return calls_[i]; }

    // First/last call for a seq, or nullptr — lets a test assert "the right
    // label reached the store" without caring how many retries happened.
    const LabelCall* find(uint32_t seq) const {
        for (size_t i = 0; i < label_count_; ++i) {
            if (calls_[i].seq == seq) {
                return &calls_[i];
            }
        }
        return nullptr;
    }

    // Fail the next n setLabel calls (failNext(999) == effectively always).
    void failNext(int n) { fail_next_ = n; }

private:
    static constexpr size_t kMaxCalls = 16;
    LabelCall calls_[kMaxCalls]{};
    size_t label_count_ = 0;
    int set_label_calls_ = 0;
    int fail_next_ = 0;
    uint32_t next_seq_ = 1;
};

}  // namespace bw_confirm_test
