#pragma once

// bw/eventlog/subscribers.h — F17 spec §6 D1: the fixed-capacity fan-out list.
// Consumers that want "event-published notifications" (F17 §3: F10, F14, F19,
// F12 optional) attach here once at boot; the logger calls each subscriber's
// publish() AFTER a successful append (§15.2 D4), so every subscriber sees the
// canonical record — final seq, boot_id — in append order.
//
// Reentrancy rule (§15.2 D5): fanout() must be called WITHOUT the logger's
// critical section held — bw::CriticalSection is a non-recursive spinlock on
// target and a subscriber's publish() may take its own guard (e.g. writing a
// LatestValue). The logger's drain() respects this.

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>

namespace bw::eventlog {

// [EXPERIMENTAL] Why 8: §15.4 — four named consumers (F10/F14/F19/F12) plus
// margin; fixed capacity keeps D1 allocation-free and the list honest when a
// fifth consumer asks for a planned extension.
inline constexpr size_t kMaxEventSubscribers = 8;

class Subscribers {
public:
    // Attach one consumer. false for nullptr or a full list (§15.5: the
    // composition root sees the failure and decides). Attaching the same
    // pointer twice would double-deliver; the list rejects duplicates too, so
    // a repeated P4 call stays idempotent.
    bool add(bw::IEventSink* subscriber);

    // Deliver `event` to every subscriber, in attach order, OUTSIDE any lock.
    // Returns how many rejected (Err != Ok) — a rejection is counted by the
    // logger, never retried (§10) and never blocks the others.
    size_t fanout(const bw::Event& event) const;

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

private:
    bw::IEventSink* list_[kMaxEventSubscribers] = {};
    // Written last in add() (pointer store before the count bump), so a reader
    // without a lock may momentarily miss a just-attached subscriber but can
    // never read a torn entry. Attach is a boot-time, single-threaded action.
    size_t count_ = 0;
};

}  // namespace bw::eventlog
