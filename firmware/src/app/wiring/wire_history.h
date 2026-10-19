#pragma once

// firmware/src/app/wiring/wire_history.h — F16's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_history.* per F16 spec §2
// Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).
// F16 creates NO task (§6 D10): the composition calls historyTick() from the
// existing low-priority ServicePump at 2 s while restoring, then at
// kAggTickPeriodMs (60 s) for steady aggregation.

#include <bw/history/daily_summary.h>
#include <bw/history/history_aggregator.h>
#include <bw/history/history_config.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace app {
struct AppContext;
}

namespace f16 {

// Constructs F16's HistoryAggregator (function-local static: idempotent, no
// heap after startup) and schedules restore without scanning the log or
// reading KV synchronously. Call after F18 mounts and F17 emits Boot. The
// existing ServicePump advances restore through historyTick().
//
// `events`/`kv` may be nullptr (providers not wired yet): boot() answers
// Err::NotReady; no history is exposed before restore completes.
void wireHistoryInit(app::AppContext& ctx, bw::IEventStore* events, bw::IKeyValueStore* kv);

// Initialization result: Err::Busy while bounded restore is still in
// progress, provider/read error while a phase is retried, then the final
// restore result. Err::NotReady before wireHistoryInit().
bw::Result<void> historyBootStatus();

// True only after probe, persisted-state restore and recent-day rebuild finish.
// The composition root uses this to print an honest startup-ready marker and
// switch historyTick() from initialization cadence to the regular cadence.
bool historyReady();

// One bounded tick: while restoring, advances at most one probe/rebuild batch
// or one KV operation; after restore, drains <= kAggMaxEventsPerTick events.
// Caller paces restore at ServicePump cadence and steady aggregation at
// kAggTickPeriodMs. Err::Busy means restore continues.
bw::Result<void> historyTick();

// The aggregator instance (composition-root accessor, F08 tracker()
// pattern). Valid after wireHistoryInit(); callers must check the wiring
// ran first. Exposed for diagnostics/tests only — data consumers use the
// snapshot accessors below.
bw::history::HistoryAggregator& historyAggregator();

// Provider getter for the UI (P4): newest summary (a calendar day or a
// session summary), false when none exists yet. Read-only snapshot — the
// phone recompute from synced events stays authoritative (spec §7).
bool historyLatestDailySummary(bw::history::DailySummary& out);

}  // namespace f16
