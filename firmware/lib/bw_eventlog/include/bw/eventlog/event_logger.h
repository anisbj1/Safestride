#pragma once

// bw/eventlog/event_logger.h — F17 spec §15 (plan) / §1 responsibility:
// EventLogger : bw::IEventSink is THE entry point every feature publishes its
// events into. It standardises the record (§15.2 D10 stamping), de-dups by
// type (D7), admits into a fixed-capacity priority queue (§6 D1/D2), and on
// drain assigns nothing itself but appends through IEventStore — which owns the
// seq per the documented contract (D3) — then fans the canonical record out to
// subscribers (D4).
//
// Concurrency model (§15.2 D5): publish() may be called from any task; drain()
// runs from the one low-priority pump. All shared state moves under
// bw::CriticalSection (non-recursive on target), and NO foreign code (store
// append, subscriber publish, fault report) is ever called with that guard
// held — otherwise a subscriber writing a LatestValue would deadlock the
// spinlock. The price of releasing the guard around store->append() is that
// publish() may evict while an append is in flight, so drain() never mutates
// the queue from a stale peek: it captures head_epoch_ at peek and re-checks
// it before advancing (see the member comment).
//
// Failure model (§10): a failed append leaves the event at the queue head,
// backs off kStoreRetryBackoffMs, reports StorageDegraded once per episode;
// producers are never blocked and never wait (publish returns immediately with
// Ok or Err::Full).

#include <cstddef>
#include <cstdint>

#include <bw/clock.h>
#include <bw/eventlog/event_priority.h>
#include <bw/eventlog/event_rate_limiter.h>
#include <bw/eventlog/subscribers.h>
#include <bw/events.h>
#include <bw/fault.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace bw::eventlog {

// [EXPERIMENTAL] Why 16: §8/§11 — a fault storm (12 types, de-duped to ~1/s
// each) plus transition bursts is absorbed without blocking a producer; 16 ×
// 48 B = 768 B is the spec §11 RAM budget for the queue itself.
inline constexpr size_t kEventQueueDepth = 16;

// [EXPERIMENTAL] Why 500 ms: §10 "retry with backoff" — caps a failed store
// at 2 append attempts/s so the low-priority pump cannot spin on a full or
// broken log, while recovery latency stays half a second.
inline constexpr bw::TimeMs kStoreRetryBackoffMs = 500;

// [EXPERIMENTAL] Why 5 s: §15.2 D10 — a wall-clock change is a re-sync only if
// it exceeds normal second-tick/monotonic jitter (±1 s truncation per check)
// by a wide margin; a phone correcting time by less than 5 s is not worth a
// TimeSync record. Tune with F28/F29 data.
inline constexpr bw::UnixSeconds kWallResyncToleranceS = 5;

// [DERIVED] Why 17: Event::detector_id for F17's own records (Boot, TimeSync);
// convention "detector id = feature number", 17 pre-allocated to F17 by the
// integrator's registry (DEVELOPMENT.md, W1 G0).
inline constexpr uint8_t kEventlogDetectorId = 17;

// [DERIVED] Why 0xFFFFFFFF: seq is uint32_t and 0 means "none", so this is the
// largest value a store can assign — the ceiling of the D3 boot_id binary
// search over readFrom's "seq >= from_seq" predicate.
inline constexpr uint32_t kMaxSeq = 0xFFFFFFFFu;

// [DERIVED] Why 1000: TimeMs counts milliseconds (02 §2) while wall time is
// whole UnixSeconds; the D10 drift check compares monotonic elapsed against
// wall seconds, so elapsed is divided by this.
inline constexpr bw::TimeMs kMsPerSecond = 1000;

// §15.4 counters — every drop/failure path is counted, never silent (CR-004).
// Read through EventLogger::stats() (the F07 droppedEvents() pattern).
struct EventLogStats {
    uint32_t dropped_full;            // publish rejected Err::Full (queue lost nothing worse)
    uint32_t evicted_lower_priority;  // residents shed to make room for a more important event
    uint32_t rate_limited;            // de-duped repeats (§15.3 intervals) — suppressed, not lost
    uint32_t subscriber_rejected;     // a subscriber's publish returned Err — counted, not retried
    uint32_t append_retries;          // failed append attempts; each deferred by the backoff
    uint32_t store_fault_episodes;    // enter-failure transitions (one fault report each)
    uint32_t queue_high_water;        // deepest the queue ever got (capacity evidence)
};

class EventLogger final : public bw::IEventSink {
public:
    // clock / faults may be nullptr (empty composition context): wall stamping
    // and TimeSync detection then stay off (honest zeros), fault reports are
    // skipped — counters still record everything (§15.7).
    EventLogger(const bw::IClock* clock, bw::IFaultReporter* faults);

    // IEventSink — producers call this (§15.2 D3/D7/D10): stamp, de-dup,
    // admit. Returns Err::Ok when accepted or deliberately de-duped; Err::Full
    // only when the event was dropped by the overflow policy (counted).
    bw::Result<void> publish(const bw::Event& event) override;

    // §15.2 D10: publish Event{Boot} exactly once (idempotent); wire() calls
    // it. detector_id 17, confidence 1.0 (factual record, not a detection).
    void emitBoot();

    // §15.2 D3: bind the F18 store. On the FIRST non-null attach the boot_id
    // is recovered from the persisted tail (binary-search probe over
    // readFrom — the only seq read the contract offers) BEFORE store_ is
    // published to the pump, so no event can ever be stamped with a stale
    // boot id. null-safe; a later attach does not re-probe (re-probing after
    // appends would skip a boot id).
    void attachStore(bw::IEventStore* store);

    // Composition-root fast path: `last_seq_hint == 0` authoritatively means
    // empty; a positive hint identifies the persisted tail and avoids probing.
    // A failed/non-matching hinted read falls back to generic recovery.
    void attachStore(bw::IEventStore* store, uint32_t last_seq_hint);

    // §6 D1 / §15.5: fixed-capacity fan-out list; false when full/null/dup.
    bool addSubscriber(bw::IEventSink* subscriber);

    // One drain cycle (the pump paces it): re-sync check, then append→fan-out
    // until the queue is empty or the store refuses (backoff, §10). Returns
    // how many events were appended. `now_ms` is the monotonic time the pump
    // read (backoff + fallback stamping when the logger has no clock).
    size_t drain(bw::TimeMs now_ms);

    EventLogStats stats() const;
    size_t queued() const;
    uint32_t bootId() const { return boot_id_; }

private:
    void attachStore(bw::IEventStore* store, uint32_t last_seq_hint, bool has_hint);

    // §15.2 D10 wall-time re-sync detection; publishes Event{TimeSync} on
    // unknown→known and on a change beyond kWallResyncToleranceS from the
    // ticking expectation. Called only from drain() (single consumer).
    void checkTimeSync();
    void emitTimeSync(bw::UnixSeconds unix_s, bw::TimeMs now_ms);

    // Queue helpers — ALL callers hold the critical section (see file header).
    // Evict the logical (0 = oldest) resident, shifting the younger events
    // toward the head so FIFO order survives an eviction (§15.3 rule).
    void removeAt(size_t logical_index);
    size_t lowestPriorityResident() const;  // oldest among the least important

    const bw::IClock* clock_;
    bw::IFaultReporter* faults_;
    bw::IEventStore* store_ = nullptr;  // published to the pump under the guard

    bw::Event queue_[kEventQueueDepth] = {};
    size_t head_ = 0;  // logical index 0 = oldest
    size_t count_ = 0;

    // Bumped by removeAt(0) — i.e. when an eviction removes exactly the element
    // drain() is peeking outside the guard for a store->append(). drain()
    // captures it at peek and re-checks it under the guard before advancing:
    // if it moved, the evictor already shifted the queue forward and counted
    // the removal in evicted_lower_priority, so advancing would orphan the
    // element now sitting at head_ — a queued event lost with NO counter
    // incremented (§15.4: every drop path is counted). Evictions at logical
    // index > 0 do not touch the head and therefore do not bump it.
    uint32_t head_epoch_ = 0;

    EventRateLimiter rate_limiter_;  // §15.2 D7 (96 B)
    Subscribers subscribers_;        // §6 D1 (64 B)
    EventLogStats stats_{};          // §15.4 counters

    uint32_t boot_id_ = 1;         // recovered at attachStore (D3); stamped at drain
    bool recovered_ = false;       // probe ran (or store was empty)
    bool boot_emitted_ = false;    // D10 idempotence
    bool fault_reported_ = false;  // current failure episode already reported

    bw::TimeMs retry_after_ms_ = 0;  // §10 backoff horizon (drain-local, no lock)

    // D10 re-sync state: last observed wall value and the monotonic time it
    // was last seen changing (both 0 until the first known wall time).
    bool have_sync_state_ = false;
    bw::UnixSeconds last_unix_ = 0;
    bw::TimeMs sync_anchor_ms_ = 0;
};

}  // namespace bw::eventlog
