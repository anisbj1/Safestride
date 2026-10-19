# F16 — Sleep / nap history

> Track **D** · Milestone **M4** · Runs on **firmware + companion** · Contract changes only via change request.

## 1. Responsibility
Maintain longitudinal records of detected sleep and naps: on the device compact daily aggregates derived from events; on the companion full history queries and visualizations.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `IEventStore.readFrom` (events)
- `IKeyValueStore`

**Outputs (produced contracts)**
- `DailySummary` records (versioned)
- Companion history queries (day/week views)

## 4. Dependencies
- `bw_core` only; companion: local DB via F21 repository interface.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Duplicate event storage.
- Recompute detection.
- Present summaries as clinical metrics.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Where aggregation happens** *(approved in Define)*
- Simplest: Only on the phone.
- Professional/scalable: Device keeps small rolling daily summaries; phone computes full analytics.
- Trade-offs: Device summaries cost flash/logic but keep the wearable useful when unsynced.
- **Choice:** Device: last 7 daily summaries; phone: everything else.

**D2. Where `DailySummary` lives and how it persists**
- Problem: the type is an output contract of F16, but only F16 firmware consumes it today (the companion recomputes full history from raw synced events — DoD §14 makes that authoritative).
- Simplest: plain struct in `bw_history`'s own include dir, persisted as a versioned blob via `IKeyValueStore` (keys `f16.meta`, `f16.day.0`..`f16.day.6`, ≤32 chars per F18 `kKvKeyMaxLen`).
- Scalable: put `DailySummary` in a shared `bw_core` contract header so F20 could later BLE-sync device summaries to the phone.
- Trade-offs: `bw_core` gives sync-ability but spends a contract write (CR-020) on a type with no second consumer yet; contract headers are append/CR-only to change later, but moving a type there later is a mechanical CR, not a redesign.
- **Choice:** F16-owned header `bw/history/daily_summary.h`, blob = 1 schema byte + record; `kSchemaVersion` inside. **CR-020 is NOT consumed.** Reserve it: if/when F20 syncs summaries to the phone, move the type to `bw_core` via CR-020 with the exact layout below.

**D3. Incremental cursor vs full rescan**
- Problem: summaries must be derived from `IEventStore` without re-reading the whole log every tick.
- Simplest: rescan `readFrom(0)` every tick — O(log size) forever, grows without bound.
- Scalable: persist a cursor (`last_processed_seq`) in `f16.meta`; each `step()` drains a bounded batch, accumulates into an open-day struct, finalizes + persists when a later day key arrives.
- Trade-offs: cursor state can desync from the log (handled in §10); incremental logic is more code but bounded work per tick.
- **Choice:** cursor + incremental accumulate. Reproducibility from raw events is guaranteed by construction (all inputs are events) and enforced by the DoD test (§9 T5): a full recompute from `readFrom(0)` over a day's `first_seq..last_seq` must equal the incrementally built record.

**D4. Wall-time attribution (events with `t_start_unix == 0`)**
- Alternatives: (a) always bucket such events into a per-`boot_id` session summary; (b) derive wall time from the last `TimeSync` event's payload (requires F17 payload knowledge — extra coupling); (c) derive from `IClock` at read time when `event.boot_id == current boot_id`: `derived_unix = clock.unixSeconds() - (now_ms - event.t_start_ms)`.
- **Choice:** (c) first, fall back to (a). Uses only `IClock` (02 §2) — no other feature's payload layout. Derived events set flag `kFlagWallTimeDerived`; events from a different `boot_id` with no unix go to a session summary (`day_key == 0`, `boot_id` set, `wall_time_unknown` implicit). `[ASSUMED]` — the same rule must be mirrored in the companion (W5) for byte-identical recomputes.

**D5. Day keying and boundary**
- Logical day = `[kDayBoundaryHour, kDayBoundaryHour + 24 h)`; the key numbers the calendar day the logical day **ends** on: `day_key = floor((unix + (24 − kDayBoundaryHour)·3600) / 86400)` — for the 04:00 boundary that is `floor((unix + 20 h) / 86400)`, giving `[04:00, 04:00)` days. *(Correction 2026-10-15, IMPLEMENT+TEST: the PLAN-stage formula `floor((unix + kDayBoundaryHour·3600) / 86400)` yielded `[20:00, 20:00)` days — a sign slip; the semantic rule below is unchanged.)* Episodes are attributed by their **start** time: main sleep starting 23:30 belongs to the logical day that ends at 04:00 — that is why `kDayBoundaryHour` exists. An episode crossing the boundary contributes its full `duration_ms` to its start day.
- **UTC convention:** `unix` seconds are UTC (the `IClock` contract, 02 §2) and the boundary hour is applied to that UTC timeline — the device does no timezone or DST conversion; `kDayBoundaryHour = 4` is therefore 04:00 **UTC**. The companion mirror (W5) must key days on the same UTC convention, or device and phone recomputes would disagree on which day an episode belongs to.

**D6. Open question §13 resolved — definition of 'daytime sleep'**
- **Assumption `[ASSUMED]` (defaults tagged `[USER]`, runtime-tunable later):** `daytime_sleep_ms` = summed `duration_ms` of counted episodes whose **start** falls in the wall-clock window `[kDaytimeStartHour, kDaytimeEndHour)` = `[09:00, 21:00)`. Any `EpisodeClass` counts (a MainSleep starting at 10:00 after a night shift *is* daytime sleep). Clinical validity of the window is out of scope (DEC-019); it is a mechanical, replaceable definition behind two named constants.

**D7. Which events feed a summary** (aggregated by `Event.type` + `detector_id` values only — never by including another feature's headers)
- `SleepEpisode` (detector_id 8, F08): closed episodes only (payload flags bit `kFlagOpenMarker` == 0 — layout mirrored as F16-local decode constants, see note below); class from payload[1], degraded share from payload[2]; `recovered` episodes (flags bit1) count as `Uncertain` with 0 ms. Open-marker events are skipped (incomplete; their close event will arrive). A `SleepEpisode` carrying any **other** `detector_id` is skipped entirely — not counted, not anchoring `first_seq` — enforced by the F16-local constant `kSleepDetectorIdMirror = 8` (D7 mirror), so a future second producer can never be double-counted with mis-decoded bytes.
- `NapCompleted`: `nap_count++`. `NapPlanned` deliberately uncounted (planned-but-cancelled is noise).
- `DrowsinessEvent` / `SuspectedCataplexy` / `AlertRaised`: not in v1 record (budget 32 B); noted for a future schema bump if history views need them.
- Confirmation comes from the event's `label` (F14 writes via `IEventStore.setLabel`), not from a separate event.
- *Payload-layout note:* F08's payload indices/flag bits live in `bw_sleep`'s config header, which F16 must not include (01 §1.1). F16 defines local constants `kSleepPayloadFlags/Class/ShareIndex = 0/1/2`, `kSleepFlagOpen = 0x01`, `kSleepFlagRecovered = 0x02` with a comment citing F08's header. `[ASSUMED]` mirror; if F08's layout ever changes, this is a detectable drift item for H9. (Moving those constants to `bw_core` would be a contract write — not justified now; noted as optional CR content together with CR-020.)

**D8. Label changes after a day is finalized**
- Problem: F14 can `setLabel` on an episode after its day was accumulated; the cursor will not re-read it.
- Simplest: accept staleness forever on device. Better: rebuild the most recent `kRebuildDays` finalized days from the event log at every `boot()` (their `first_seq..last_seq` ranges are stored, so the rescan is bounded). Live confirmations during runtime update only the current open day.
- **Choice:** rebuild-at-boot for `kRebuildDays`. `[ASSUMED]` known limitation: an older day's `confirmed_count` can be stale **on device**; the companion recompute from synced events (with labels) is authoritative. Tested in T8.

**D9. Clock regression (day_key moves backwards)**
- Rule: finalize the open day with what it has, open a new slot with the new key; two slots may share a `day_key`; increment a `clock_regressions` counter in `f16.meta`. The increment is persisted **immediately** through the same meta write path (not deferred to the next finalize), so a regression on the final tick before power-down still lands on disk; if that write fails, the increment is rolled back and the triggering event is retried (RAM == disk). Deterministic, lossless; the companion merge is authoritative. Forward jumps of days finalize intermediate days as seen (no synthetic empty days). Tested in T6.

**D10. Execution model / wiring seam**
- `HistoryAggregator` is pure logic plus injected read-only providers (`const IEventStore*`, `IKeyValueStore*`, `const IClock*` — constructor injection, same pattern as F08's tracker), fully host-testable with `bw_testkit` fakes. No FreeRTOS/Arduino calls, no heap.
- `wire_history.{h,cpp}` constructs it and exposes `wireHistoryInit()` (boot/restore) and `historyTick()` for the integrator to call from a task (StorageTask or app tick) at `kAggTickPeriodMs`; the wiring-time `boot()` result is stored and **reported** (not re-run) through `historyBootStatus()`, so the seam's comment and behaviour agree; F16 does not create its own task and does not call itself from `app.cpp` (integrator's H7 step).
- F16 emits **no events** this wave; `detector_id = 16` stays unused (reserved).

## 7. Module structure
```
firmware/lib/bw_history/
  include/bw/history/daily_summary.h   DailySummary (32 B, layout in D2-below), flags, static_asserts
  include/bw/history/history_config.h  all named constants (§8), KV key names, payload-decode constants (D7)
  include/bw/history/history_aggregator.h
  src/history_aggregator.cpp
firmware/src/app/wiring/wire_history.h / .cpp     seam per D10 (only src/app/ knows concrete classes)
firmware/test/bw_history/test_bw_history/         Unity, native (§9)
```
API sketch (implementation may add accessors, not change signatures):
```cpp
class HistoryAggregator {
 public:
  HistoryAggregator(const HistoryConfig&, const IEventStore*, IKeyValueStore*, const IClock*);
  Result<void> boot();                     // load meta+slots, rebuild last kRebuildDays days
  Result<void> step();                     // drain <= kAggMaxEventsPerTick; finalize/persist as needed
  size_t summaries(DailySummary* out, size_t max) const;  // ring snapshot oldest->newest
};
struct DailySummary {                     // exactly 32 B, trivially copyable (static_assert)
  static constexpr uint8_t kSchemaVersion = 1;
  uint32_t day_key;          // floor((unix + (24 − kDayBoundaryHour)*3600)/86400); 0 = session summary
  uint32_t boot_id;          // session summaries: the boot; calendar days: 0
  uint32_t first_seq;        // earliest contributing event — recompute anchor (DoD §14)
  uint32_t last_seq;         // latest contributing event
  uint32_t total_sleep_ms;   // summed duration of counted episodes
  uint32_t daytime_sleep_ms; // D6 definition
  uint32_t main_sleep_ms;    // EpisodeClass::MainSleep share
  uint8_t  flags;            // bit0 kFlagWallTimeDerived
  uint8_t  episode_count;    // closed SleepEpisode events (recovered -> Uncertain, 0 ms)
  uint8_t  nap_count;        // NapCompleted events
  uint8_t  confirmed_count;  // episodes whose store label == Confirmed
};                            // KV blob = 1 schema byte + 32 B = kSummaryBlobBytes (33)
```
Companion half (W5, needs F21's repository interface — not implemented now): `HistoryRepository` + day/week queries over synced events mirroring D4–D7 attribution rules; must consume the same golden fixture (shared test data under `companion/lib/features/history/`) so device and phone recomputes agree byte-for-byte. If F20 later syncs device summaries instead of recomputing, that is the moment CR-020 moves `DailySummary` to `bw_core`. *(W5: superseded and specified in detail in §15 — no new repository class; the view reads F21's `EventRepository` read-only.)*

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kDeviceSummaryDays` | 7 | `EXPERIMENTAL` | Bounds flash; phone holds the long history |
| `kDayBoundaryHour` | 4 | `USER` | A 'day' that ends at 04:00 groups night sleep sensibly (D5) |
| `kDaytimeStartHour` | 9 | `USER` | D6 daytime-window start; default `[ASSUMED]`, tunable via F18 settings later |
| `kDaytimeEndHour` | 21 | `USER` | D6 daytime-window end; default `[ASSUMED]` |
| `kSecPerDay` | 86400 | `DERIVED` | Day-key arithmetic |
| `kAggMaxEventsPerTick` | 8 | `DERIVED` | 8×48 B = 384 B batch keeps static RAM < 1 KB (§11); event rate is a few dozen/day, so the log drains within ~1 h of ticks after a backlog |
| `kAggTickPeriodMs` | 60000 | `DERIVED` | Aggregation is not latency-critical; 1 min bounds worst-case staleness of the open day |
| `kRebuildDays` | 2 | `DERIVED` | Two rebuild days covers the realistic confirmation delay (D8) at trivial boot cost |
| `kSummaryBlobBytes` | 33 | `DERIVED` | 1 schema byte + 32 B record; unknown schema ⇒ discard (§10) |
| `kSleepPayloadFlags/Class/ShareIndex` | 0/1/2 | `DERIVED` | D7 mirror of F08 layout (cannot include `bw_sleep`) |
| `kSleepFlagOpen` / `kSleepFlagRecovered` | 0x01 / 0x02 | `DERIVED` | D7 mirror of F08 flags |
| `kSleepDetectorIdMirror` | 8 | `ASSUMED` | D7: F08's producer id mirrored from `bw/sleep/sleep_config.h` (`kSleepEpisodeDetectorId`); non-matching `SleepEpisode` events are skipped (cannot include `bw_sleep`; drift is an H9 review item) |
| `kSeqMax` | `UINT32_MAX` | `DERIVED` | Seq-exhaustion sentinel: `cursor_ + 1` at `kSeqMax` would wrap to 0 and replay the whole log (double counting); `step()`/probe/rebuild readers early-out on it (§10) |

KV keys (≤ 32 chars, F18 charset): `f16.meta`, `f16.day.0` … `f16.day.6`.
All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan (`pio test -e native`, `firmware/test/bw_history/test_bw_history/`, Unity + `bw_testkit`: `InMemoryEventStore`, `InMemoryKeyValueStore`, `FakeClock`)
- **T1 `test_aggregation_basic`** — happy path from §spec examples: fixture of SleepEpisode/NapCompleted events → expected `total/daytime/main_sleep_ms`, counts, `first/last_seq`.
- **T2 `test_day_boundary`** — episode 23:50→07:10 lands in the day ending 04:00; events exactly at the 04:00 boundary; multi-day episodes; `kDaytimeStartHour`/`EndHour` inclusive/exclusive edges.
- **T3 `test_unknown_wall_time`** — all-unknown unix ⇒ per-`boot_id` session summaries with `day_key == 0`; clock-derived fallback works within the same `boot_id` (sets `kFlagWallTimeDerived`); other `boot_id` ⇒ session summary; separate sessions per boot.
- **T4 `test_cursor_persistence`** — simulated reboot (new aggregator, same KV): cursor restored, no double counting; empty store; exactly one event; store read error ⇒ tick skipped, no crash (§10).
- **T5 `test_reproducibility` (DoD §14)** — for a multi-day fixture: run incrementally with tiny ticks, then recompute each day independently from raw events over its `first_seq..last_seq` via `readFrom` and compare **byte-for-byte**; repeat after `setLabel` changes + reboot (rebuild path must produce the same result as a full recompute).
- **T6 `test_clock_regression`** — D9: forward multi-day jump finalizes intermediates; backward jump finalizes + reopens, `clock_regressions` increments, no data lost.
- **T7 `test_ring_eviction`** — 8+ days ⇒ only `kDeviceSummaryDays` kept, oldest evicted; `summaries()` ordering oldest→newest.
- **T8 `test_confirmed_refresh`** — setLabel on a finalized day within `kRebuildDays` ⇒ `confirmed_count` updates at next `boot()`; older day stays stale (asserted deliberately, documents D8 limitation).
- **T9 `test_no_heap`** — allocation hook: zero allocations during steady-state `step()` (pattern: `bw_ml/test_no_heap`).
- **T10 `test_incremental_batching`** — >`kAggMaxEventsPerTick` backlog drains over multiple ticks, cursor advances monotonically, result equals one-shot recompute.
- **T11 `test_kv_corruption`** — corrupt blob / unknown `kSchemaVersion` in `f16.meta` or a day slot ⇒ discard that blob and full-rescan (§10); valid slots survive.
- **T12 `test_wiring_compile`** — `wire_history.h/.cpp` compile-only check (pattern: `bw_sleep/test_wiring_compile`).
- **T13 `test_foreign_detector_episode_not_counted`** — D7 detector filter: a closed `SleepEpisode` from another producer (`detector_id != 8`) is skipped entirely (count, `first_seq`, `total_sleep_ms` unaffected).
- **T14 `test_stale_boot_event_never_fabricates_day`** / **`test_missing_boot_event_never_fabricates_day`** — D4 anchor guards: a missing Boot event for the newest boot (stale anchor) or no Boot event at all ⇒ every event stays a session summary, never a clock-derived day.
- **T15 `test_kv_read_error_fails_boot_without_discarding`** — §10: a KV `get` error (not `NotFound`/`Corrupt`) fails `boot()` with `booted_` false, `step()` retries it, and after the store heals the persisted record is intact (read errors are NOT the corrupt-discard path).
- **T16 `test_kv_slot_write_failure_retries_finalize`** / **`test_kv_meta_write_failure_lands_day_once`** — §10 write-failure paths: `put` failure ⇒ `Err`, RAM unchanged, triggering event unconsumed, day lands exactly once on the healthy retry (RAM, disk, and after reboot).
- **T17 `test_full_ring_crash_window_replay_no_duplicate`** — crash-window dedupe: slots landed + meta failed with a FULL ring ⇒ reboot replay finalizes the replayed day by REPLACE, no duplicate identity+`first_seq` slot.
- **T18 `test_seq_exhaustion_cursor_never_wraps`** — a persisted cursor at `kSeqMax` never wraps `cursor_ + 1` to 0 (no whole-log replay).
- **T19 `test_clock_regression_persisted_on_last_finalize`** — D9 immediate persist: a regression on the last tick before power-down survives a fresh `boot()`.
- Companion (W5): tests **C1–C9** in §15.6 (pytest, colocated under `companion/lib/features/history/tests/`) — implemented wave 5: 26 cases, all passing.

## 10. Failure behavior
- Unknown wall time ⇒ per-`boot_id` session summary, `day_key == 0` (§6 D4); never guess a date.
- `readFrom`/`get`/`put` error ⇒ tick returns `Err`, aggregator state unchanged, retried next tick; no fault event emitted (not health-critical; F23 does not need it).
- **At `boot()` specifically:** a `get` error other than `Err::NotFound`/`Err::Corrupt` (blob states) fails `boot()` with `booted_` left false — it is a retried failure, **not** the corrupt-discard path (discarding would silently lose wall-time-derived days a rescan cannot regenerate, since that boot's clock mapping is gone). `step()` re-runs `boot()` until the store heals; only absent/corrupt blobs trigger discard + full rescan.
- Seq exhaustion: `cursor_ == kSeqMax` ⇒ `step()` returns `Ok` without reading (`cursor_ + 1` would wrap to 0 and replay the whole log); the probe and rebuild readers apply the same early-out.
- Corrupt or unknown-schema KV blob ⇒ discard that blob; cursor loss ⇒ full rescan from seq 0 (rebuild), which is always correct because summaries are event-derived (DoD).
- Clock regression ⇒ D9 rule; forward jumps ⇒ finalize-as-seen, no synthetic days.
- Unknown / insufficient data stays a valid output: an empty day yields no slot at all, never a zeroed fabricated summary (00 §1).

## 11. Budget
- RAM (static, worst case): read batch 8×48 B = 384 B + ring 7×32 B = 224 B + open-day accumulator ≈ 40 B + meta ≈ 16 B ⇒ **≈ 664 B < 1 KB** (spec DoD kept).
- Flash: code ~2–3 KB `[EXPERIMENTAL]`; persistent data 7×33 B + meta ≈ 270 B in LittleFS KV.
- CPU: ≤ a few events per 60 s tick; negligible vs. detection pipeline.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Summaries reproducible from events alone — enforced by test T5 (byte-for-byte recompute from raw events).

## 15. Companion half (W5) — plan
> Planned at PLAN stage, implemented at IMPLEMENT+TEST (see status log 2026-10-17): no production code outside this section's paths. Scope per the DEC-005 W5 interim ruling ("nothing fancy"): a minimal, **read-only** history view over the local synced SQLite DB — days, sleep-episode segments, naps. No charts, no GUI framework, no new DB schema or writer, no week rollup.

### 15.1 What the view reads (explicit delta vs. the §7 sketch)
- The SQLite DB is F21's: only `companion/lib/data/event_repository.py` touches it (schema v2, events keyed `(device_id, seq)`, 48 B raw decoded via the CR-002 layout). F16's companion half adds **no schema and no writer** — it is read-only over `EventRepository.timeline()` pages (the repository's public seam; F16 never writes, asserted by test C7).
- Events consumed: closed `SleepEpisode` (type 0, `detector_id` 8 mirror) and `NapCompleted` (type 9) under the D7 rules, mirrored as F16-local constants (§15.5). `NapPlanned` is never counted. F16 emits no events; `detector_id` 16 stays unused.
- **Wall time (delta):** the §7 sketch predates F21's SYNC §4.5 reconstruction. The phone's authoritative wall time is the DB's `wall_unix` column — set at insert from `t_start_unix`, else resolved from the boot's TimeSync events (`domain/time_reconstruct.py`); 0 = unknown, never fabricated. The companion does **not** repeat the device's D4(c) `IClock` derivation (device-only). The invariant both sides share is D4's core rule: unknown wall time ⇒ per-`boot_id` session summary (`day_key == 0`), never a guessed date.
- **Day keying (delta):** same UTC formula as D5/D6, mirrored as pure functions over `wall_unix` (the device's `kFlagWallTimeDerived` events appear on the phone as TimeSync-resolved `wall_unix`; parity holds when both land on the same unix second — guaranteed by the shared golden fixture, C1).
- User-facing wording is reused from F21's `domain/wording.py` (detections = "possible/suspected" + confidence; unknown enum values render `unknown (#n)`; "unknown/insufficient data" stays a valid output).

### 15.4 Design decisions
**D11. Presentation form.**
- Problem: render history while the DEC-005 W5 ruling forbids anything fancy and defers the GUI (Flutter) decision.
- Simplest: CLI console renderer, same CLI-first pattern as F21's `timeline` command. Scalable: a real GUI (Flutter per DEC-005).
- Trade-offs: GUI is the eventual path but is exactly the framework decision the ruling deferred; the renderer split (all text formatted in `view_model.py` rows) means any future UI consumes the same view-model unchanged.
- **Choice:** console renderer now; zero new dependencies; view-model/renderer separation keeps the GUI swap a presentation-only change.

**D12. Read seam: page F21's repository vs. feature-local SQL.**
- Simplest & chosen: wrap the public `EventRepository.timeline(limit, since_seq, device_id)` seam, paging until exhausted, filtering types in F16's own layer. No SQL, no schema knowledge, no second repository class.
- Alternative: feature-local `SELECT` over the `events` table — fewer bytes moved, but duplicates schema knowledge, bypasses the seam and breaks silently on any F21 schema migration (single-writer/one-reader rule).
- Trade-off: paging moves non-history rows too; at "a few dozen events/day" (§8) a month is a handful of `HISTORY_PAGE_SIZE` pages — noise, not cost.
- **Choice:** repository-seam wrapper, read-only by construction (test C7).

**D13. Attribution mirror (D4/D5/D6 reconciliation).**
- The phone recomputes from `wall_unix` (SYNC §4.5) instead of the device's `IClock` derivation; `wall_unix == 0` ⇒ session summary (`day_key == 0`, grouped per `boot_id`) — device D4's fallback rule verbatim. Day key `floor((wall_unix + (24 − kDayBoundaryHour)·3600)/86400)` (D5, UTC) and daytime window on start hour `[kDaytimeStartHour, kDaytimeEndHour)` (D6) are mirrored as pure functions with the spec's own constants. An empty day yields no row; never a zeroed fabricated summary (§10).

**D14. Aggregation state: recompute vs cursor.**
- The device needs a cursor (D3) because it is bounded; the phone is not. **Choice:** pure full recompute from the DB on every invocation — no persisted phone-side state, so the D8 stale-`confirmed_count` limitation is device-only (labels are always fresh; the companion recompute is already the authoritative side per D8) and a whole class of desync/corruption bugs cannot exist.

**D15. Golden-fixture sharing.**
- §7 requires device and phone to consume the same fixture. **Choice:** `golden_fixture.json` — contract-level event field tables (48 B layout + TimeSync points) plus expected per-day summaries (day_key, first/last_seq, totals, counts). The pytest decodes via F21's codec; the firmware T5 fixture carries identical numbers. `bw_history` tests are FROZEN this wave, so parity this wave is **value-level** (same JSON numbers vs. the C++ fixture constants), maintained manually and re-checked at H9 review; a generator emitting the C array from the JSON is deferred until a second consumer exists. `[ASSUMED]`

**D16. Wording/labels.**
- Episode rows reuse `domain/wording`: `detection_label(type, confidence)` ("possible sleep episode (confidence 0.87)"), class/label via `format_enum` over local `EpisodeClass`/`UserLabel` name mirrors; unknown times render "unknown"; no banned diagnostic terms in any output (test C8). Summaries are never presented as clinical metrics (§5).

### 15.5 Constants (named, tagged, justified — mirror of §8)
| Name | Value | Tag | Why |
|---|---|---|---|
| `DAY_BOUNDARY_HOUR` | 4 | `USER` | §8 `kDayBoundaryHour` mirror: same UTC `[04:00, 04:00)` days as the device |
| `DAYTIME_START_HOUR` | 9 | `USER` | D6 daytime-window start (default `[ASSUMED]`) |
| `DAYTIME_END_HOUR` | 21 | `USER` | D6 daytime-window end (default `[ASSUMED]`) |
| `SEC_PER_DAY` | 86400 | `DERIVED` | D5 day-key arithmetic |
| `SEC_PER_HOUR` | 3600 | `DERIVED` | boundary offset `(24 − 4)·3600` s |
| `EVENT_TYPE_SLEEP_EPISODE` / `EVENT_TYPE_NAP_COMPLETED` | 0 / 9 | `HW` | data_decision §4.5 append-only `EventType` indices |
| `SLEEP_DETECTOR_ID_MIRROR` | 8 | `ASSUMED` | D7 mirror of F08's producer id; non-matching episodes skipped entirely |
| `SLEEP_PAYLOAD_FLAGS_INDEX` / `SLEEP_PAYLOAD_CLASS_INDEX` / `SLEEP_PAYLOAD_SHARE_INDEX` | 0 / 1 / 2 | `DERIVED` | D7 payload-layout mirror |
| `SLEEP_FLAG_OPEN` / `SLEEP_FLAG_RECOVERED` | 0x01 / 0x02 | `DERIVED` | D7 flag mirror |
| `USER_LABEL_CONFIRMED` | 1 | `HW` | data_decision §4.5 `UserLabel::Confirmed` |
| `EPISODE_CLASS_NAMES` | `{0 Uncertain, 1 Rest, 2 Nap, 3 MainSleep}` | `HW` | data_decision §4.5 `EpisodeClass` mirror |
| `DAY_KEY_SESSION` | 0 | `DERIVED` | day_key 0 = unknown wall time, per-boot session summary |
| `HISTORY_PAGE_SIZE` | 200 | `DERIVED` | one `timeline()` page; ≥3 weeks at a few dozen events/day ⇒ 1–2 queries, bounds memory |
| `MAX_HISTORY_EVENTS` | 5000 | `DERIVED` | fetch safety cap for a pathological DB; output says "truncated", oldest events dropped — never fabricated |
| `DEFAULT_HISTORY_DAYS` | 30 | `EXPERIMENTAL` | default CLI window, `--days` overridable; no synthetic empty days |

### 15.6 Tests (pytest, colocated in `companion/lib/features/history/tests/`; gates `python -m pytest lib/features/history/tests -q` from `companion/` + `python -m ruff check companion`)
- **C1 `test_golden_parity`** — `golden_fixture.json` → decode → aggregate → expected per-day summaries equal (day_key, first/last_seq, totals, counts). Phone mirror of DoD §14 / firmware T5.
- **C2 `test_day_boundary`** — 23:50→07:10 episode lands in the day ending 04:00; exact-04:00 boundary events; daytime `[09:00, 21:00)` inclusive/exclusive edges; day-key epoch spot-check.
- **C3 `test_unknown_wall_time_session_summaries`** — `wall_unix == 0` events group per `boot_id` with `day_key == 0`; separate sessions per boot; never a rendered date; empty day renders no row.
- **C4 `test_episode_decode_mirror`** — `detector_id != 8` skipped entirely (count, totals, seq range unaffected); open-marker skipped; recovered → Uncertain 0 ms; class/degraded-share decoded at the mirror indices.
- **C5 `test_nap_counting`** — `NapCompleted` counted and listed; `NapPlanned` ignored.
- **C6 `test_confirmed_label_fresh`** — label == Confirmed counted; a re-seeded label change is reflected on the next invocation (no cursor staleness, D14).
- **C7 `test_read_only`** — seeded in-memory DB: full row snapshot identical before/after a view build (the seam is never written through).
- **C8 `test_rendered_wording`** — episode text is "possible sleep episode (confidence …)"; unknown times render "unknown"; no `BANNED_DIAGNOSTIC_TERMS` in any rendered output.
- **C9 `test_empty_and_unsupported`** — empty DB → "no history yet", exit 0; unsupported-schema rows are skipped, never decoded as data.
- **C10 `test_whole_package_collection_is_clean`** (F16-AUD-1) — subprocess `python -m pytest companion --collect-only -q` from the worktree root: exit 0, no "errors during collection", `collected >= 121` (94 F21 + 26 F16 + C10 itself at this tip; `>=` because F20/F22 suites add more), and both suites' node-id prefixes present — so the natural whole-package invocation can never regress silently again.

### 15.7 Budget
No new dependencies, no threads, no DB writes. Working set: one page of records (`HISTORY_PAGE_SIZE` = 200 rows ≈ tens of KB of Python objects) plus O(days) aggregates — negligible on desktop. Runtime O(events in window); a 30-day view costs 1–2 repository queries. Ruff-clean under F21's rule set (≤100-char lines).

### 15.8 Out of scope / registry
- Week rollup, charts, GUI, runtime tuning of the `[USER]` hours, confirm/reject actions (F14 labels are read only), export (F28), pairing/sync (F19/F20).
- **CR-024 stays allocated and UNUSED**: the view reads existing contracts only — no detector_id, no FaultCode, no schema_id, no contract change needed.

---
