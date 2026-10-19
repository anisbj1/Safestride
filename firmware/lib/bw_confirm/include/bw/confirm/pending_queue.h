#pragma once

// bw/confirm/pending_queue.h — F14's bounded wait queue (spec §7, D1).
//
// Fixed-capacity ring of events waiting to be prompted. The overflow rule is
// spec §9: when full, the OLDEST waiting entry is dropped and handed back to
// the caller, which finalizes it with an Unlabeled marker (never a silent
// overwrite — the caller needs the dropped seq to publish the marker).

#include <cstddef>
#include <cstdint>

#include <bw/confirm/confirm_config.h>
#include <bw/events.h>
#include <bw/types.h>

namespace bw::confirm {

// One queued event. Kept to 16 B (TimeMs first so the struct packs with no
// padding, the CR-002 lesson from Event): plan §11 budgets 4 x 16 B.
struct PendingEntry {
    TimeMs received_ms;  // monotonic time the event arrived (event's t_start_ms)
    uint32_t seq;        // F17-assigned seq — what setLabel() targets
    EventType type;
};

class PendingEventQueue {
public:
    // [EXPERIMENTAL] (spec §8) — the queue size is the spec's kMaxPending.
    static constexpr size_t kCapacity = kMaxPending;

    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == kCapacity; }
    size_t size() const { return count_; }
    void clear();

    // Appends at the tail (newest end). When the queue was full the oldest
    // entry is evicted and returned via `evicted`; returns true iff a drop
    // happened (Err::Overflow sense: something was lost, caller records it).
    bool push(const PendingEntry& entry, PendingEntry& evicted);

    // Prepends at the head (oldest end) — used when an active prompt is
    // withdrawn back to waiting (step/alert edge). Same drop-oldest rule when
    // full; returns true iff a drop happened.
    bool pushFront(const PendingEntry& entry, PendingEntry& evicted);

    // Removes and returns the oldest entry (used when it is offered).
    bool pop(PendingEntry& out);

    // Oldest entry without removing it; nullptr when empty.
    const PendingEntry* front() const;

    // Removes the first entry with the given seq (user labeled a waiting
    // entry); false when absent.
    bool removeBySeq(uint32_t seq, PendingEntry& out);

private:
    size_t physical(size_t logical) const {
        return (static_cast<size_t>(head_) + logical) % kCapacity;
    }

    PendingEntry entries_[kCapacity]{};
    uint8_t head_ = 0;   // logical index 0 == oldest
    uint8_t count_ = 0;  // number of live entries, <= kCapacity
};

}  // namespace bw::confirm
