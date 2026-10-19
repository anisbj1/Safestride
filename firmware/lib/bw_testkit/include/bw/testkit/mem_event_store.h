#pragma once

#include <bw/testkit/host_only.h>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace bw::testkit {

// [EXPERIMENTAL] Store capacity. Why 64: at the expected event rate (a few state
// changes per hour, plus faults) 64 events covers an overnight host-test scenario;
// sync/replay tests that stream thousands instantiate a bigger
// InMemoryEventStore<...>. When full: Err::Full — CR-004's own example
// ("IEventStore::append when the append-only log is full") — refused, nothing
// written. The real F18 store ring-overwrites *synced* segments; that is F18
// policy, deliberately not guessed at here.
constexpr size_t kDefaultEventStoreCapacity = 64;

// InMemoryEventStore — bw::IEventStore fake (02 §5). Documents the semantics
// F18's LittleFsEventStore and F17's EventLogger must satisfy:
//  - append() assigns the next seq (starting at 1; seq 0 means "none", matching
//    lastSyncedSeq() == 0) and stores a copy with seq filled in;
//  - readFrom() serves every stored event with seq >= from_seq, oldest first,
//    up to `max`; a cursor beyond the end is an ordinary "nothing yet" -> Ok, 0;
//  - setLabel() of an unknown seq -> Err::NotFound;
//  - markSyncedThrough() is monotonic and idempotent (sync protocol 02 §6.3).
template <size_t Capacity = kDefaultEventStoreCapacity>
class InMemoryEventStore final : public bw::IEventStore {
    static_assert(Capacity > 0, "store needs capacity for at least one event");

public:
    bw::Result<uint32_t> append(const bw::Event& event) override {
        if (count_ >= Capacity) {
            return {bw::Err::Full, 0};  // CR-004: refused, nothing written, seq not consumed
        }
        bw::Event stored = event;
        stored.seq = next_seq_++;
        events_[count_] = stored;
        ++count_;
        return {bw::Err::Ok, stored.seq};
    }

    bw::Result<size_t> readFrom(uint32_t from_seq, bw::Event* out, size_t max) override {
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
        bw::Event* event = find(seq);
        if (event == nullptr) {
            return {bw::Err::NotFound};
        }
        event->label = label;
        return {bw::Err::Ok};
    }

    bw::Result<void> markSyncedThrough(uint32_t seq) override {
        if (seq > last_synced_) {
            last_synced_ = seq;  // monotonic; replaying an older ack changes nothing
        }
        return {bw::Err::Ok};
    }

    uint32_t lastSyncedSeq() const override { return last_synced_; }

    size_t count() const { return count_; }
    uint32_t nextSeq() const { return next_seq_; }

    // Precondition: i < count().
    const bw::Event& at(size_t i) const { return events_[i]; }

    void clear() {
        count_ = 0;
        next_seq_ = 1;
        last_synced_ = 0;
    }

private:
    bw::Event* find(uint32_t seq) {
        for (size_t i = 0; i < count_; ++i) {
            if (events_[i].seq == seq) {
                return &events_[i];
            }
        }
        return nullptr;
    }

    bw::Event events_[Capacity];
    size_t count_ = 0;
    uint32_t next_seq_ = 1;  // seq 0 is reserved for "none" (lastSyncedSeq, read cursors)
    uint32_t last_synced_ = 0;
};

}  // namespace bw::testkit
