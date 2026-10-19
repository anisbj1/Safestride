#pragma once

// bw/history/history_aggregator.h — F16's device-side aggregation (spec §6
// D2–D10). Pure logic plus injected read-only providers: no FreeRTOS/Arduino
// calls, no heap, host-testable with bw_testkit fakes (§6 D10).
//
// Lifecycle: construct once per boot (wiring), call boot() to restore, then
// step() periodically — kAggTickPeriodMs (§8). boot() may also be retried by
// step() itself after a failure (§10: a failed read is retried next tick).
//
// Correctness invariant (DoD §14): every persisted DailySummary is derived
// from IEventStore events alone — nothing else is stored, and a full
// recompute from raw events reproduces each record byte-for-byte (test T5).

#include <cstddef>
#include <cstdint>

#include <bw/clock.h>
#include <bw/events.h>
#include <bw/history/daily_summary.h>
#include <bw/history/history_config.h>
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/types.h>

static_assert(bw::history::kDeviceSummaryDays <= 10,
              "history_config dayKvKey encodes one decimal digit");

namespace bw::history {

// [USER] hours are runtime-tunable later via F18 settings (spec §8); the
// defaults are the tagged constants in history_config.h. Out-of-range hours
// are the caller's contract violation — the aggregator clamps them to
// [0, 23] at construction rather than mis-keying days.
struct HistoryConfig {
    uint8_t day_boundary_hour = kDayBoundaryHour;
    uint8_t daytime_start_hour = kDaytimeStartHour;
    uint8_t daytime_end_hour = kDaytimeEndHour;
};

class HistoryAggregator {
public:
    // Providers may be null (wiring before F18 lands): boot()/step() then
    // answer Err::NotReady — an honest "not available", never a guess (§10).
    // `events` is a NON-const pointer because 02 §5's IEventStore::readFrom
    // is a non-const member; the read-only discipline is by convention —
    // F16 never calls append/setLabel/markSyncedThrough (§6 D10).
    HistoryAggregator(const HistoryConfig& cfg, IEventStore* events, IKeyValueStore* kv,
                      const IClock* clock);

    // Start resumable restore. Each step() advances one bounded probe/rebuild
    // batch or one KV operation; Err::Busy means initialization continues, and
    // summaries remain unavailable until ready(). A corrupt or unknown-schema
    // blob discards persisted state and causes an event rescan (§10). A KV read
    // error does not discard anything; step() retries that restore phase.
    Result<void> boot();

    // One bounded tick: drain <= kAggMaxEventsPerTick events, finalize and
    // persist as day keys change, persist the meta cursor. Atomic per event:
    // a provider failure leaves the failing event unconsumed and is retried
    // next tick; no event is ever double-counted (a crash between the slot
    // write and the meta write is reconciled by first_seq dedupe on replay).
    Result<void> step();
    bool ready() const { return booted_; }

    // Ring snapshot, oldest -> newest. Returns how many were written.
    size_t summaries(DailySummary* out, size_t max) const;
    size_t summaryCount() const { return booted_ ? ring_count_ : 0; }
    // Newest record (a day or a session summary); false when the ring is empty.
    bool latestDailySummary(DailySummary& out) const;

    // §6 D9 diagnostics: how many backward day-key jumps were observed.
    uint32_t clockRegressions() const { return clock_regressions_; }
    uint32_t lastProcessedSeq() const { return cursor_; }
    // Boot id learned from Boot events / the boot-time probe; 0 = unknown.
    uint32_t currentBootId() const { return current_boot_id_; }

private:
    // Slot identity: a calendar day (day_key != 0) or a per-boot session
    // summary (day_key == 0, boot_id set — §6 D4 fallback, §10).
    struct Identity {
        bool session;
        uint32_t day_key;
        uint32_t boot_id;
    };

    uint32_t dayKeyFor(UnixSeconds unix) const;
    Identity identityFor(const Event& e, bool& derived, UnixSeconds& resolved_unix) const;
    static bool isCounting(const Event& e);  // SleepEpisode (closed) or NapCompleted
    void accumulate(DailySummary& s, const Event& e, bool derived, UnixSeconds start_unix) const;
    static bool sameIdentity(const DailySummary& s, const Identity& id);
    static bool sameIdentity(const DailySummary& a, const DailySummary& b);

    const DailySummary* newestDayRef() const;  // newest DAY record in the ring
    Result<void> finalizeOpen();               // close the open day/session (§6 D5/D9)
    Result<void> persistSlots(const DailySummary* records, uint8_t count);  // f16.day.0..n-1
    Result<void> persistMeta(uint8_t count);  // f16.meta (cursor = safe replay point)
    Result<void> probeCurrentBootId();        // last Boot event's boot_id (read-only scan)
    Result<void> advanceBoot();
    Result<void> rebuildDayBatch();  // §6 D8, one bounded range batch
    void beginBoot();

    enum class BootPhase : uint8_t { Idle, Probe, Meta, Slots, Rebuild, Ready };
    BootPhase boot_phase_ = BootPhase::Idle;
    uint32_t probe_from_ = 1;
    uint32_t probe_last_boot_id_ = 0;
    uint32_t probe_max_boot_id_ = 0;
    bool probe_exhausted_ = false;
    uint32_t saved_cursor_ = 0;
    uint8_t saved_count_ = 0;
    uint8_t slot_index_ = 0;
    bool meta_ok_ = false;
    size_t rebuild_index_ = 0;
    size_t rebuilt_count_ = 0;
    uint32_t rebuild_from_ = 0;
    DailySummary rebuild_loaded_{};
    DailySummary rebuild_fresh_{};
    bool rebuild_active_ = false;
    bool rebuild_have_ = false;
    bool rebuild_range_done_ = false;
    Err boot_result_ = Err::Ok;

    HistoryConfig cfg_;
    IEventStore* events_;  // read-only by convention (see the ctor note)
    IKeyValueStore* kv_;
    const IClock* clock_;

    DailySummary ring_[kDeviceSummaryDays] = {};  // finalized records, oldest -> newest
    uint8_t ring_count_ = 0;
    DailySummary open_ = {};  // in-progress record; identity lives in day_key/boot_id
    bool open_valid_ = false;

    uint32_t cursor_ = 0;             // last seq consumed from the log
    uint32_t clock_regressions_ = 0;  // §6 D9, persisted in f16.meta
    uint32_t current_boot_id_ = 0;    // sticky per instance (one instance = one boot)

    // §6 D4 boot anchor: clock derivation is only trusted for events that come
    // after the last Boot event of a KNOWN, not-stale anchor. Stale = events
    // in the log carry a boot_id newer than the last Boot event's (the current
    // boot's Boot event is missing), so current_boot_id_ describes an OLDER
    // boot and deriving through the current clock would fabricate a day (§10).
    uint32_t last_boot_seq_ = 0;      // seq of the last Boot event seen
    bool boot_anchor_known_ = false;  // false: no Boot event in the log (or probe failed)
    bool boot_anchor_stale_ = false;  // true: newest boot in the log lacks its Boot event

    bool booted_ = false;
};

}  // namespace bw::history
