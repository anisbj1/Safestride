#pragma once

// bw/ble/event_notifier.h - F19 spec 7 (EventNotifier row) + spec D7: the
// pure newest-event logic behind the Event Notify characteristic. Reads only
// the bw::IEventStore contract (never F17/F18 internals), host-tested with
// bw_testkit's InMemoryEventStore.
//
// D7 semantics: on the FIRST subscription it baselines to the store tail (no
// history burst), then each poll() advances forward and hands back only the
// NEWEST event - at most one notification per poll. Intermediate events are
// deliberately skipped: completeness is the sync path's job (external.md 6.2
// calls Event Notify "convenience only").

#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/types.h>

namespace bw::ble {

class EventNotifier {
public:
    explicit EventNotifier(IEventStore* store = nullptr);

    // Composition-root binding (wire_ble attach seam); nullptr = the notifier
    // counts store_misses and hands back nothing - never fabricated events.
    void bind(IEventStore* store);

    // CCCD toggled on: first call baselines to the current tail so a newly
    // subscribed phone sees only events from NOW on (spec D7).
    void subscribe(TimeMs now_ms);

    // CCCD toggled off: subsequent polls do no store reads at all (spec 9).
    void unsubscribe();

    bool subscribed() const { return subscribed_; }

    // One CommsTask tick: false = nothing to notify (unsubscribed, no store,
    // store error - skipped silently per spec 9 but COUNTED - or no new
    // event). true = `out` holds the newest event to notify.
    bool poll(TimeMs now_ms, Event& out);

    // Diagnostics (spec 10): store errors skipped, events beyond the newest
    // per poll skipped, baseline scan chunks spent.
    uint32_t storeErrors() const { return store_errors_; }
    uint32_t skippedIntermediate() const { return skipped_intermediate_; }

private:
    // Advances last_notified_seq_ to the current tail without producing
    // notifications (first subscription, and error recovery).
    void baseline();

    // [EXPERIMENTAL] Scan/poll batch: how many events one IEventStore::readFrom
    // call may return. 4 lets a caught-up notifier swallow a short backlog in
    // ONE poll (newest wins, intermediates counted - spec D7) while the scan
    // buffer stays a static-storage member (4 x 48 B in the lib's RAM budget),
    // never a task-stack array (00 7.2).
    static constexpr size_t kScanChunkEvents = 4;

    IEventStore* store_;
    bool subscribed_;
    bool baselined_;
    uint32_t last_notified_seq_;
    uint32_t store_errors_;
    uint32_t skipped_intermediate_;
    Event scan_buf_[kScanChunkEvents] = {};
};

}  // namespace bw::ble
