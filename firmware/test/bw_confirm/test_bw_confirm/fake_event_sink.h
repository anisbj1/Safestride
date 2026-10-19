#pragma once

// firmware/test/bw_confirm/test_bw_confirm/fake_event_sink.h — fake IEventSink
// (plan §7): captures the label-change notifications F14 publishes (D5) so a
// test can assert type / detector_id / payload without F17 in the picture.

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace bw_confirm_test {

class FakeEventSink : public bw::IEventSink {
public:
    bw::Result<void> publish(const bw::Event& e) override {
        if (fail_next_ > 0) {
            --fail_next_;
            return {bw::Err::Full};
        }
        if (count_ < kMaxEvents) {
            events_[count_] = e;
            ++count_;
        }
        return {bw::Err::Ok};
    }

    size_t count() const { return count_; }
    const bw::Event& at(size_t i) const { return events_[i]; }

    // First published event of a type, or nullptr.
    const bw::Event* firstOfType(bw::EventType type) const {
        for (size_t i = 0; i < count_; ++i) {
            if (events_[i].type == type) {
                return &events_[i];
            }
        }
        return nullptr;
    }

    void failNext(int n) { fail_next_ = n; }

private:
    static constexpr size_t kMaxEvents = 16;
    bw::Event events_[kMaxEvents]{};
    size_t count_ = 0;
    int fail_next_ = 0;
};

// Reads back D5's little-endian seq from a published notification's payload.
inline uint32_t payloadSeq(const bw::Event& e) {
    uint32_t seq = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        seq |= static_cast<uint32_t>(e.payload[i]) << (8U * i);
    }
    return seq;
}

}  // namespace bw_confirm_test
