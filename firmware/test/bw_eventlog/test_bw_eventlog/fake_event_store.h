#pragma once

// f17_test::FakeEventStore — the F17 suite's own bw::IEventStore fake
// (spec §15.6; the brief requires it inside firmware/test/bw_eventlog/**).
// bw_testkit::InMemoryEventStore documents the contract semantics but has no
// fault injection, and F17's §9/§10 tests need failed appends, failed reads
// and call counts. Semantics copied from bw_testkit (mem_event_store.h:20-27):
//   - append() ASSIGNS the next seq (from 1) and stores a copy with seq
//     filled; Err::Full refuses and consumes no seq;
//   - readFrom() serves every event with seq >= from_seq, oldest first, up to
//     max; past-the-end is Ok/0 (the §15.2 D3 boot_id probe relies on this);
//   - storage is a plain array that OUTLIVES any single EventLogger, so a
//     "reboot" (new logger over the same store) resumes where the last one
//     stopped (§9 simulated reboots).

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/testkit/host_only.h>
#include <bw/types.h>

namespace f17_test {

// Re-entrancy hook for FakeEventStore::append (audit M-1). The store calls it
// at the TOP of append(), i.e. after drain() has peeked the queue head outside
// the critical section but before the record is written — the exact window in
// which publish() can evict that head. The hook runs on the drain's own call
// stack, so a test can re-enter the logger under test and force the race
// deterministically on a single-threaded host (no timing, no threads).
class AppendHook {
public:
    virtual ~AppendHook() = default;
    virtual void onAppend() = 0;
};

// [EXPERIMENTAL] Why default 64: the §9/§15.6 suites hold one logical boot's
// records (queue depth 16 per drain plus pre-seeded history); tests needing a
// larger log pass it explicitly (e.g. FakeEventStore<512> for the 500-event
// pre-seed). Static array only — no heap, host-only.
template <size_t Capacity = 64> class FakeEventStore final : public bw::IEventStore {
public:
    bw::Result<uint32_t> append(const bw::Event& event) override {
        ++append_calls_;
        if (append_hook_ != nullptr) {
            append_hook_->onAppend();  // may re-enter the logger under test
        }
        if (fail_next_appends_ > 0) {
            --fail_next_appends_;
            ++failed_appends_;
            return {fail_err_, 0};
        }
        if (fail_all_appends_ || count_ >= Capacity) {
            ++failed_appends_;  // CR-004 Full: refused, nothing written, seq not consumed
            return {bw::Err::Full, 0};
        }
        bw::Event stored = event;
        stored.seq = next_seq_++;
        events_[count_] = stored;
        ++count_;
        return {bw::Err::Ok, stored.seq};
    }

    bw::Result<size_t> readFrom(uint32_t from_seq, bw::Event* out, size_t max) override {
        ++read_calls_;
        if (fail_reads_ > 0) {
            --fail_reads_;
            return {bw::Err::BusError, 0};
        }
        if (max > 0 && out == nullptr) {
            return {bw::Err::InvalidArg, 0};
        }
        size_t written = 0;
        for (size_t i = 0; i < count_ && written < max; ++i) {
            if (events_[i].seq >= from_seq) {
                out[written] = events_[i];
                ++written;
            }
        }
        return {bw::Err::Ok, written};
    }

    bw::Result<void> setLabel(uint32_t seq, bw::UserLabel label) override {
        for (size_t i = 0; i < count_; ++i) {
            if (events_[i].seq == seq) {
                events_[i].label = label;
                return {bw::Err::Ok};
            }
        }
        return {bw::Err::NotFound};
    }

    bw::Result<void> markSyncedThrough(uint32_t seq) override {
        if (seq > last_synced_) {
            last_synced_ = seq;
        }
        return {bw::Err::Ok};
    }

    uint32_t lastSyncedSeq() const override { return last_synced_; }

    // --- fault injection ------------------------------------------------------
    // The next n appends fail with `err`; nothing is written, no seq consumed.
    void failNextAppends(size_t n, bw::Err err = bw::Err::Full) {
        fail_next_appends_ = n;
        fail_err_ = err;
    }
    void failAllAppends(bool on) { fail_all_appends_ = on; }
    // The next n readFrom calls fail (D3 probe error path).
    void failReads(size_t n) { fail_reads_ = n; }

    // Re-entrancy hook: called at the top of every append() until replaced
    // with nullptr (AppendHook's contract above).
    void setAppendHook(AppendHook* hook) { append_hook_ = hook; }

    // --- observation ----------------------------------------------------------
    size_t appendCalls() const { return append_calls_; }
    size_t readCalls() const { return read_calls_; }
    size_t failedAppends() const { return failed_appends_; }
    size_t count() const { return count_; }
    uint32_t nextSeq() const { return next_seq_; }
    // Precondition: i < count().
    const bw::Event& at(size_t i) const { return events_[i]; }

private:
    bw::Event events_[Capacity]{};
    size_t count_ = 0;
    uint32_t next_seq_ = 1;  // seq 0 = "none" (bw_testkit convention)
    uint32_t last_synced_ = 0;

    bool fail_all_appends_ = false;
    size_t fail_next_appends_ = 0;
    bw::Err fail_err_ = bw::Err::Full;
    size_t fail_reads_ = 0;

    size_t append_calls_ = 0;
    size_t read_calls_ = 0;
    size_t failed_appends_ = 0;

    AppendHook* append_hook_ = nullptr;  // raw, non-owning: the test owns it
};

// A producer-shaped event: only the fields a producer owns (type, time,
// detector_id, confidence); seq/boot_id/schema stay 0 for F17 to fill (§15.2).
inline bw::Event makeEvent(bw::EventType type, bw::TimeMs t_ms, uint8_t detector_id = 0) {
    bw::Event event{};
    event.type = type;
    event.t_start_ms = t_ms;
    event.detector_id = detector_id;
    event.confidence = 0.5f;
    return event;
}

}  // namespace f17_test
