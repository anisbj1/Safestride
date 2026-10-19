# F08 — Sleep episode detection

> Track **C** · Milestone **M3** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Track sustained sleep-like patterns from the state stream and emit structured `SleepEpisode` events (start, duration, confidence, quality, context), distinguishing normal inactivity, rest, nap/sleep and uncertain data.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `StateSnapshot` stream
- `IClock` (day/night classification when wall time known)
- Planned-nap info via `StateSnapshot.mode == Nap` (no F15 dependency)

**Outputs (produced contracts)**
- `Event{SleepEpisode}` via `IEventSink` (at episode close, plus open marker)
- Episode status for UI (`Idle/Candidate/Active`) as `LatestValue`

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Compute alertness itself.
- Store data (use `IEventSink`).
- Label episodes as clinical events.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Episode boundaries — the state machine**
- Problem: `PossibleSleep` flickers (brief awakenings, sensor blips). Without structure, every blip becomes an event and one night fragments into many; with too little structure, short sleep-like spans cannot be told apart from drowsiness.
- Simplest: one open flag — start when state is `PossibleSleep`, end when `Awake`. No extra state, but a 40 s blip becomes an "episode" and there is no way to say "not sure yet".
- Most scalable: Candidate → Active with min duration, merge short gaps, close after sustained recovery, quality-aware confidence — plus a *paused* state for wear interruptions.
- Trade-offs: the machine costs 3 states, 2 `[EXPERIMENTAL]` constants and one span counter (~40 B, all host-testable); a paused state needs a third timer and re-implements F07's `NotWorn` hysteresis inside F08.
- **Choice:** Candidate/Active machine with gap merging (config), **no pause state** — `NotWorn`/`Unknown` are ordinary gap time. Transition table (first match wins):

| # | From | Condition | To | Side effect |
|---|---|---|---|---|
| 1 | any | snapshot repeats the previous `t_ms` | unchanged | no-op (de-duplication) |
| 2 | Idle | `alertness == PossibleSleep` | Candidate | record `t_start`, `t_start_unix`, zero counters |
| 3 | Candidate | sleep-like span ≥ `kMinEpisodeMs` with every gap ≤ `kMergeGapMs` | Active | publish **open-marker** event, write the open marker (D4) |
| 4 | Candidate | gap > `kMergeGapMs` (any reason, incl. `NotWorn`/`Unknown`) | Idle | nothing published: drowsiness/inactivity, not an episode |
| 5 | Active | state returns to `PossibleSleep` within `kMergeGapMs` | Active | same episode continues |
| 6 | Active | gap > `kMergeGapMs` | Idle | publish **close** event, clear the marker |
| 7 | Active | `wear == NotWorn` when row 6 fires | Idle | as row 6, plus `reasons |= DeviceNotWorn` |
| 8 | Active | `t_ms` < previous `t_ms` (time discontinuity) | Idle | publish close event, class `Uncertain`, `reasons |= SensorUnavailable` |

- Row 2 as implemented (also refines the "gap" sentence below): a snapshot starts a candidate only when it is *sleep-like* — `alertness == PossibleSleep` **with both source qualities usable**; a `PossibleSleep` snapshot whose IMU or PPG is `Insufficient` is ordinary gap time (D5 counts it as degraded), never a start. Starting also samples the episode's evidence context: `t_start_unix` and `activity` once, at candidate start; `mode` follows the snapshot and is refreshed while sleep-like steps continue (rows 3/5).
- A **gap** is time between snapshots whose `alertness` is not `PossibleSleep`. All time maths use `bw::elapsedMs()` / `bw::withinMs()` (`bw/time.h`, saturating) so a stale or pre-reset timestamp can never inflate a duration — this replaces the "TimeMs wrap" case of §9 (uint64 does not wrap in this product's lifetime, `bw/types.h`).
- Duration = span (`t_close − t_start`), merged gaps included.
  - Simplest alternative: count only sleep-like time (subtract gaps) — closer to "time asleep", but needs per-segment bookkeeping. Most scalable: per-segment accumulation with per-segment quality.
  - **Choice:** span — the gap is bounded by `kMergeGapMs` (≤ 2 min error `[EXPERIMENTAL]`) and history/UI want a wall span.
- §13 closed — **sleep while the device is not worn**: starting depends on F07's gate (its `NotWorn` priority already keeps `alertness != PossibleSleep` when unworn), so F08 does **not** re-check `wear` at row 2 — a second wear check here would duplicate F07. A removal longer than `kMergeGapMs` closes the episode (rows 6/7) and the observed part is **kept**, never discarded. Alternative — discard the partial episode: rejected, it throws away data that was actually observed.

**D2. Classification — `Uncertain` / `Rest` / `Nap` / `MainSleep`**
- Problem: §1 wants normal inactivity, rest, nap/sleep and uncertain data distinguished. Duration alone cannot tell a daytime rest from a night sleep, but wall time is often unknown (DEC-009: `unixSeconds() == 0` until the phone syncs), and nothing must ever be claimed that the data does not support.
- Simplest: duration only (`< kNapMaxMs` → `Nap`, else `MainSleep`) — one always-available input, but labels a 40 min afternoon lie-down a "nap" and has no word for doubt.
- Most scalable: duration + time of day + confidence + quality share + personal baseline (F13) — needs a timezone/night window and a learned profile that do not exist in any contract yet.
- Trade-offs: time of day needs a timezone the contracts do not carry; a night-window constant would be a second, unjustified source of truth next to `OperatingMode` (F24 already derives day/night from wall time when it knows it).
- **Choice:** duration and quality rules first, `OperatingMode` as the context axis, wall time only as a known/unknown gate — **no timezone arithmetic in F08**. Inputs: span `duration_ms`, `degraded_share` (D5), `StateSnapshot.mode`, wall time known at candidate start. Rule table (first match wins):

| # | Condition | Class | Example |
|---|---|---|---|
| 1 | `degraded_share > kMaxDegradedShare` | `Uncertain` | 50 min span, PPG dead for 62 % of it |
| 2 | `duration_ms ≥ kNapMaxMs` | `MainSleep` | 4 h 10 min overnight, phone never synced |
| 3 | `mode == Nap` (F15 planned-nap signal) | `Nap` | 35 min after StartNap, clock unknown |
| 4 | wall time unknown (`unixSeconds() == 0`) | `Uncertain` | 70 min in mode `Day`, no sync yet |
| 5 | `mode == Night` | `Nap` | 1 h 20 min night fragment (short of `kNapMaxMs`) |
| 6 | otherwise (wall time known, `Day`/`Calibration`/`Research`) | `Rest` | 40 min afternoon lie-down |

- Wall time is sampled once at candidate start through an optional `const IClock*`; `nullptr` ⇒ unknown (the device `IClock` implementation is still open, F00 §13 — F08 must not assume it exists).
- Ordering is deliberate: duration beats mode (row 2 before row 3: a 4 h planned nap is recorded as `MainSleep`), intent beats the clock (row 3 before row 4: a planned nap needs no sync).
- Spans shorter than `kMinEpisodeMs` never become events, so "normal inactivity" is represented by the state stream, not by a `SleepEpisode`.
- `Uncertain` is a first-class result, not a failure (00 §7.5).
- Examples for DoD §14: 40 min `Day` + synced clock → `Rest` · 35 min `mode == Nap`, no clock → `Nap` · 4 h 10 min, no clock → `MainSleep` · 70 min `Day`, no clock → `Uncertain` · 50 min with 62 % degraded → `Uncertain` · 4 min (short of `kMinEpisodeMs`) → no episode at all · reboot-recovered → `Uncertain` (D4).

**D4. Reboot recovery — where the open marker lives (§10)**
- Problem: §10 requires the next boot to emit a recovered-episode event, but after a reboot F08 has no memory, and `IEventStore` has no "find my last event" call to read the open-marker event back.
- Simplest: no persistence — a reboot silently drops every interrupted episode. Cheap and honest, but contradicts §10 and loses exactly the long episodes that matter.
- Most scalable: a durable journal of episode segments read back at boot (`IEventStore` extension + CR) — full fidelity, but a contract change plus flash writes per segment.
- **Choice:** one schema-versioned record under `IKeyValueStore` key `kMarkerKey`, written once when the episode is promoted to Active (never at Candidate, so blips cost no flash writes) and cleared at close; `recover()` reads it once per boot. Optional dependency: `kv == nullptr` ⇒ no recovery, still no crash (graceful degradation, 00 §5). Cost: 2 KV writes per episode.
  - `[ASSUMED]` — §5 says "Store data (use `IEventStore`… use `IEventSink`)", read here as *"no private storage backend"*: episode **data** is only ever published through `IEventSink`, while the marker is a liveness flag through a `bw_core` contract. Confirm with the integrator (§13).
  - Recovered event: `class = Uncertain`, payload flag `kFlagRecovered`, `duration_ms = 0` and `t_start_ms = 0` (the marker's monotonic times belonged to the previous boot — reusing them under the new `boot_id` would be a lie), `t_start_unix` from the marker (0 if never synced), `confidence = 0.0f`, `quality` = `Insufficient` on both sources, `reasons |= SensorUnavailable`. Consumers learn "a sleep-like episode was open before the reboot; start/duration unknown" and nothing more.
- **PLAN (W2, 2026-10-12):** built exactly as §7 sketches — optional `const IKeyValueStore*` parameter defaulting to `nullptr`; the W2 wiring passes `nullptr` (F18 lands in W3), so this wave requires **no storage backend at all**; recovery is host-tested with `bw_testkit::InMemoryKeyValueStore`. The open-marker *event* is emitted through `IEventSink` (§3), independent of whether a KV store is wired. The `[ASSUMED]` above stands (§13); no CR was raised.

**D5. Confidence and degraded share**
- Problem: an episode's confidence must fall when the data behind it was poor, and "more degraded data than `kMaxDegradedShare` ⇒ `Uncertain`" needs a share that is honest under an irregular snapshot cadence.
- Simplest: confidence = mean of the per-snapshot confidences; share = count of bad snapshots ÷ total count.
- Most scalable: per-source weighted quality (IMU vs PPG separately, F23 `SourceQuality`) combined with F07 confidence and a learned reliability prior — more accurate, but F08 has no contract that says how to weight two sources, and inventing one would fabricate precision.
- Trade-offs: time-weighting costs two `uint32_t` accumulators (~8 B) but stays correct when the poll rate changes; count-based silently biases the share when cadence changes mid-episode.
- **Choice:** time-weighted. Per step, `Δt = bw::elapsedMs(t_ms, previous_t_ms)` is added to `total_ms`, and to `sleep_like_ms` only when `alertness == PossibleSleep` **and** neither source is `Insufficient`; `degraded_share = 1 − sleep_like_ms / total_ms` (`total_ms == 0` ⇒ share 0, not NaN). Final `confidence = mean_confidence × (1 − degraded_share)`, clamped to `[0,1]`. Both the formula and `kMaxDegradedShare` are `[EXPERIMENTAL]`; time we did not observe sleep (gaps, `NotWorn`, `Unknown`) counts as degraded — honest, never invented.

| Byte | Content |
|---|---|
| 0 | flags: `kFlagOpenMarker` (`0x01`, set = the episode is still open), `kFlagRecovered` (`0x02`) |
| 1 | `EpisodeClass` numeric value (append-only; open marker carries the *provisional* class, close carries the final one) |
| 2 | degraded share in percent (0–100) |
| 3–7 | reserved, written as 0 |

- `Uncertain == 0` in the enum is deliberate: a zeroed payload decodes as `Uncertain`, the honest default (00 §7.5). Note for the integrator: F16/F28 need this table; it is not a CR, just a hand-off.

> **Wording rule (docs/00_PROJECT_CONTEXT.md):** F08 emits *possible / suspected* sleep-like episodes — the input is F07's `PossibleSleep` state and every emitted `Event` carries a `confidence` in `[0,1]`. Nothing produced here is a clinical label (§5), and a `Uncertain` class or `Insufficient` quality is reported as-is rather than guessed away.

## 8. Constants (named, tagged, justified)
All live as members of `SleepEpisodeConfig` (§7) unless stated.

| Name | Value | Tag | Why |
|---|---|---|---|
| `kMinEpisodeMs` | 300000 | `EXPERIMENTAL` | Shorter spans are recorded as drowsiness, not sleep |
| `kMergeGapMs` | 120000 | `EXPERIMENTAL` | Brief awakenings do not split an episode |
| `kNapMaxMs` | 10800000 | `EXPERIMENTAL` | Longer episodes classified as main sleep |
| `kMaxDegradedShare` | 0.5f | `EXPERIMENTAL` | More degraded data than this ⇒ class `Uncertain` |
| `kMarkerKey` | `"f08.open"` | `DERIVED` | `IKeyValueStore::get/put` take `const char*` keys; the `f08.` prefix namespaces F08 inside the shared store (§6 D4) |
| `kSleepEpisodeDetectorId` | 8 | `DERIVED` | `Event::detector_id`, derived from the convention "detector id = feature number" (§6 D8); the convention itself is `[ASSUMED]`, see §13 |
| `kFlagOpenMarker` | 0x01 | `DERIVED` | `payload[0]` bit: episode still open (§6 D7) |
| `kFlagRecovered` | 0x02 | `DERIVED` | `payload[0]` bit: emitted by `recover()` after a reboot (§6 D7) |
| `kMarkerSchemaVersion` | 1 | `DERIVED` | First byte of the stored marker; a mismatch means "ignore the marker" instead of decoding garbage |
| `kSleepPollPeriodMs` | 1000 | `DERIVED` | `wire_sleep.h`; pump period = the 1 s `StateTask` cadence of `runtime.md` §3.2 — snapshots arrive at that rate, faster polling only hits the row-1 no-op; `vTaskDelayUntil` period |
| `kSleepPumpPriority` | 2 | `DERIVED` | `wire_sleep.h`; W2 convention caps new core-1 pumps at ≤ 3; 2 sits below the ceiling so a late episode pump can never delay a higher-numbered neighbour — episodes are second-granularity, max lateness is a diagnostic (runtime.md §3.2 rules), not a failure |
| `kSleepPumpStackBytes` | 2048 | `EXPERIMENTAL` | `wire_sleep.h`; the pump only polls, steps and publishes (no buffers, no formatting); re-measure with `uxTaskGetStackHighWaterMark` at Validate and adjust deliberately |
| `kPendingEventCapacity` | 8 | `DERIVED` | `wire_sleep.cpp`; bounded FIFO for events produced before `attachEventSink` runs (§7, M-1) — at most 1 recovery event per boot plus a pathological pre-attach burst; drop-oldest, never blocks the machine (§10) |

Boundary semantics to pin in tests: `kMinEpisodeMs` and `kMergeGapMs` are **inclusive** (`≥` / `≤` promote or merge; one millisecond beyond splits).
No `TimeMs` value is ever truncated to 32 bits (01 §4); percentages are `uint8_t` 0–100 only inside `payload[2]`.

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
`firmware/test/bw_sleep/test_bw_sleep/` · Unity · `pio test -e native`, layout mirrors `test/bw_core/test_bw_core/`.
Files: `test_main.cpp` (runner) · `test_classifier.cpp` · `test_tracker_boundaries.cpp` · `test_recovery.cpp` · `test_events_and_status.cpp`.
Fakes: `bw_testkit`'s `FakeClock` (wall time known/unknown — starts at `unixSeconds() == 0`, tests call `setUnixSeconds`) and `InMemoryKeyValueStore` (`bw/testkit/mem_kv_store.h`; renamed 2026-10-12 in this plan — no `MemKvStore` exists in `bw_testkit`, grep-verified, no test weakened). The fake does not count writes, so `test_bw_sleep/` adds a ~10-line `CountingKvStore` decorator (owned path, like the sink below) for the `put`-count assertions; corrupt markers are seeded by `put`-ting raw garbage bytes. `bw_testkit` has **no** `IEventSink` double, so `test_bw_sleep/` carries its own ~20-line recording sink (owned path). Scripted `StateSnapshot` sequences with explicit `t_ms` — no real sleeping.

- **Classifier** (`test_classifier.cpp`): every row of the §6 D2 table, both clock states; boundaries `kNapMaxMs −1/=/+1 ms`; `kMaxDegradedShare` at exactly `0.5` (passes) and `0.5 + ε` (`Uncertain`); `mode == Nap` with an unsynced clock.
- **Tracker** (`test_tracker_boundaries.cpp`): every row of the §6 D1 table in order; `kMinEpisodeMs −1 ms` ⇒ still Candidate, `= kMinEpisodeMs` ⇒ Active; gap exactly `kMergeGapMs` merges, `+1 ms` splits; two merges in a row stay one episode; `NotWorn` interruption shorter/longer than the gap (kept vs closed with `DeviceNotWorn`); identical `t_ms` twice ⇒ one event; `t_ms` going backwards ⇒ row 8.
- **Recovery** (`test_recovery.cpp`): marker present ⇒ exactly one recovered event (`Uncertain`, `kFlagRecovered`, `duration_ms == 0`); marker absent ⇒ none; corrupt/`kMarkerSchemaVersion` mismatch ⇒ none; `recover()` twice ⇒ one event; `kv == nullptr` ⇒ none, no crash; marker cleared after `recover()` and after every normal close; `CountingKvStore` `put` count == 1 open + 1 close per Active episode (candidates cost no writes).
- **Events/status** (`test_events_and_status.cpp`): `type`, `detector_id == kSleepEpisodeDetectorId`, `schema_version == kSchemaVersion`, open marker flags, provisional vs final class bytes, `duration_ms` span across merged gaps, `t_start_unix` captured at candidate start (0 when the clock never synced), `status_changed` only on phase/`t_start` change (guards `LatestValue::overwrites()`).
- **W2 wiring configuration (both deps `nullptr`)** — the exact wave-2 build (`kv == nullptr`, `wall_clock == nullptr`): the full happy-path episode plus every boundary row above still pass; `recover()` yields no event; classification treats wall time as unknown (row 4 of the D2 table can fire). The pump itself is platform glue (`vTaskDelayUntil`, core 1) and is deliberately not host-tested; it is checked at Validate / H7. Its poll→step→deliver body is `pumpOnce()`, host-driven by `test_delivery_order.cpp`.
- **Wiring delivery order (audit M-1; §7 attach-order independence)** — `test_delivery_order.cpp`: drives the real wiring path (`AppContext` mailbox → `pumpOnce()` → shared tracker → `deliver`/flush) in BOTH attach orders; the attach-late case fails if the pending-event buffer is reverted (H10-verified).
- **Budget**: report `sizeof(SleepEpisodeTracker)` in the status log against §11; `static_assert` the contract structs stay trivially copyable.

Duration accuracy over `TimeMs` discontinuity and the reboot boundary is covered by the "backwards `t_ms`" and `recover()` cases above (uint64 `TimeMs` does not wrap in this product's lifetime — `bw/types.h`).

## 10. Failure behavior
- Reboot mid-episode ⇒ next boot `recover()` reads `kMarkerKey` and emits one recovered-episode event flagged `Uncertain` (`kFlagRecovered`), exactly as specified in §6 D4. No store wired, no marker, or a schema mismatch ⇒ no event and no fault: a missing marker is the normal case, not an error.
- `t_ms` discontinuity (row 8) ⇒ close the episode with `Uncertain` + `SensorUnavailable` rather than emit a wrong duration.
- Stale state stream ⇒ no new `t_ms`, so nothing advances: `duration_ms` freezes at the last observed snapshot and wall time is never used for timing. F08 never invents time from an unsynced clock.
- `IEventSink::publish()` returns `Err::Full`/`Overflow` ⇒ bookkeeping still completes and the marker is cleared; F08 does not retry into a busy log (the drop is already counted by F00's/F17's counters). Trade-off: that one event is lost rather than blocking the state machine — simplest over reliable, and re-opening the marker would re-trigger recovery at the next boot for an episode that already ended.
- Storage write failure at promotion ⇒ episode still becomes Active (events keep flowing), recovery is simply unavailable — capability degrades, the device does not (00 §5).

## 11. Budget
RAM < 1 KB (tracker state + marker record + status value; no buffers, no containers).
Task stack `kSleepPumpStackBytes` = 2048 B (§8), high-water mark measured at Validate (H3/H12).
CPU: one O(1) `step()` per `kSleepPollPeriodMs` (1 s) tick; `classify()` runs once per episode close — negligible against the 1 s StateTask cadence of `runtime.md` §3.2.
`sizeof(SleepEpisodeTracker)` is reported in the status log at Implement.

## 12. Out of scope
- Sleep staging.
- Long-term aggregation (F16).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Episode classes documented with examples. — satisfied by §6 D2 (rule table + worked examples).

---
