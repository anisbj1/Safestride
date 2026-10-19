#pragma once

// bw_sync test support — FailingEventStore (F20 spec §7): an IEventStore
// wrapper that fails readFrom on the Nth call (fault injection at the store
// seam, host-testing guidance "fault injection"). Everything else forwards to the
// inner store untouched, so tests swap one object without rebuilding fakes.

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/testkit/host_only.h>

namespace f20test {

class FailingEventStore final : public bw::IEventStore {
public:
    explicit FailingEventStore(bw::IEventStore& inner) : inner_(inner) {}

    // Arm the fault: the Nth readFrom call (1-based) returns Err::BusError.
    // -1 disarms. Calls are counted across the whole wiring lifetime, so tests
    // arm relative to a known counter via readCalls().
    void failReadAt(int nth_call) { fail_read_at_ = nth_call; }
    void clearFault() { fail_read_at_ = -1; }
    int readCalls() const { return read_calls_; }

    bw::Result<std::uint32_t> append(const bw::Event& event) override {
        return inner_.append(event);
    }

    bw::Result<std::size_t> readFrom(std::uint32_t from_seq, bw::Event* out,
                                     std::size_t max) override {
        ++read_calls_;
        if (fail_read_at_ >= 0 && read_calls_ == fail_read_at_) {
            return {bw::Err::BusError, 0};  // injected store failure (F11)
        }
        return inner_.readFrom(from_seq, out, max);
    }

    bw::Result<void> setLabel(std::uint32_t seq, bw::UserLabel label) override {
        return inner_.setLabel(seq, label);
    }

    bw::Result<void> markSyncedThrough(std::uint32_t seq) override {
        return inner_.markSyncedThrough(seq);
    }

    std::uint32_t lastSyncedSeq() const override { return inner_.lastSyncedSeq(); }

private:
    bw::IEventStore& inner_;
    int fail_read_at_ = -1;
    int read_calls_ = 0;
};

}  // namespace f20test
