# F17 — Event logging

> Track **D** · Milestone **M3** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Standardize how every important occurrence becomes an `Event`: sequence numbers, boot ids, timestamps (monotonic + wall when known), de-duplication, rate limiting, and routing to storage and notifiers.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `Event` from any feature via `IEventSink`
- `IClock`
- `IEventStore` (F18)

**Outputs (produced contracts)**
- Persisted events with `seq`/`boot_id` filled
- Event-published notifications (consumers: F10, F14, F19, F12 optional)
- `Event{Boot}` and `Event{TimeSync}` records

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Implement storage formats.
- Contain detection logic.
- Block producers when storage is slow (queue with drop policy).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Fan-out**
- Simplest: Direct calls to each consumer.
- Professional/scalable: Small subscriber list with fixed capacity.
- Trade-offs: Static list is simple and avoids dynamic allocation.
- **Choice:** Fixed-capacity subscriber array.

**D2. Overflow policy**
- Simplest: Block.
- Professional/scalable: Drop lowest-priority, keep counters, emit a synthetic `dropped` note.
- Trade-offs: Loses low-priority info but keeps real-time tasks alive.
- **Choice:** Priority-aware drop with counters.

## 7. Module structure
- `EventLogger : IEventSink`
- `EventPriority` table
- `Subscribers`
- `EventRateLimiter`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kEventQueueDepth` | 16 | `EXPERIMENTAL` | Bursts (e.g. fault storms) absorbed without blocking |
| `kMinRepeatIntervalMs (per type)` | table | `EXPERIMENTAL` | Prevents log flooding by repeating faults |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Ordering and seq monotonicity across simulated reboots (fake store persists last seq).
- Rate limiter and overflow policy tests.
- Wall time unknown → `t_start_unix = 0`, boot id preserved.

## 10. Failure behavior
- Store failure ⇒ keep in RAM queue, retry with backoff, report fault.

## 11. Budget
RAM ≤ 16 × sizeof(Event) ≈ 768 B.

## 12. Out of scope
- Persistence layout (F18), transport (F19/F20).

## 13. Open questions / assumptions
- Which event types are 'critical' and never dropped. → **answered 2026-10-12 (PLAN): §15.3 priority table; critical = {Boot, SensorFault, AlertRaised, BatteryWarning, SuspectedCataplexy}.**
- Additional `[ASSUMED]` items introduced by the plan are listed in §15.7 (readFrom probe semantics, flash/core placement of the pump, `StorageDegraded` as the store-failure fault code).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Event type/priority table documented.

## 15. Implementation plan (wave 3, PLAN stage — 2026-10-12; no production code in this stage)

Contracts used (all pre-existing, none changed): `bw::IEventSink::publish` (bw/providers.h:80), `bw::IEventStore` (bw/providers.h:86), `bw::Event` 48 B CR-002 layout (bw/events.h), `bw::IClock`, `bw::CriticalSection` (bw/platform_sync.h), `bw::IFaultReporter` + existing `FaultCode::StorageDegraded` (bw/fault.h:54), `bw_testkit` fakes. **No CR needed; CR-014 stays reserved/unused.** `EventType` set unchanged (12 values, all already present).

### 15.2 Design decisions (D1/D2 already in §6)

**D4. Fan-out position**
- Problem: when do subscribers (F10/F14/F19/F12) see an event relative to storage?
- Alternatives: (a) synchronous fan-out inside `publish()`; (b) fan-out in the drain after a successful append.
- Trade-offs: (a) notifies even while storage is down, but subscribers see `seq = 0` (before assignment), two orderings must be tested (multi-producer direct calls vs queue), and an event later shed from the storage queue was already announced → inconsistent views. (b) one ordered stream, subscribers get the canonical record (final seq/boot_id); during a store outage notifications wait — exactly what §10 prescribes (events stay in the RAM queue).
- **Choice: (b).** Alerting never depends on the log (00 non-negotiable): F10's alert path is unaffected; only *notifications about* events wait for storage. Consequence: no store attached ⇒ no fan-out; the bounded queue absorbs (§15.3 policy).

**D5. Concurrency**
- `publish()` runs from any task (State/Alert/Input…), the drain from the pump. Queue, rate-limiter and counters are guarded by `bw::CriticalSection` (FreeRTOS spinlock on target, no-op on host — same pattern as `LatestValue`). The guard covers only 48 B copies and index/counter updates: `append` and fan-out run **outside** the critical section (the head is peeked under the guard, advanced only after success, so a failed append leaves the event at the head). `publish()` never blocks: admission is decided inside the guard and returns immediately.

**D7. De-dup vs priority (two independent mechanisms)**
- Rate limiting happens at admission, before queueing: per-type `kMinRepeatInterval*` on the three storm-prone types only (§15.3); a suppressed repeat returns `Ok` (the occurrence is represented by the retained event; §1 responsibility is de-dup, not loss), counted in `rateLimited`. All other types are transition events already hysteresis'd upstream (F07/F08/F10) → interval 0 = never limited.
- Priority governs overflow eviction only (§15.3). Critical events are never *evicted*; the first occurrence of a critical fault is never suppressed (intervals only bound repeats, shortest 1000 ms). Per-type (not per-fault-code) keying follows §8; trade-off: a second, different SensorFault inside 1 s is suppressed too — first kept, counters visible; refine at Validate if F28 data shows loss.

**D8. Store-failure reporting**
- §10 requires a fault report. Use existing `FaultCode::StorageDegraded` (bw/fault.h:54) via `ctx.faults` (null-safe drop while null, F01 D7 pattern), reported once per failure episode (fail→ok→fail re-reports), not per retry — retries are counted (`appendRetries`). `[ASSUMED]` semantic fit; avoids a CR on the append-only FaultCode enum.

**D9. Drop accounting instead of a synthetic "dropped" event**
- §6 D2's chosen wording is "priority-aware drop with counters". A synthetic dropped-note would need a new EventType (would consume reserved CR-014), would compete for the same 16 slots, and describes an event that was never assigned a seq. Instead: counters + `f17::stats()` accessor (f07::droppedEvents() precedent) for F23/diagnostics later. **CR-014 remains unused.**

### 15.3 Priority and rate-limit table (answers §13; DoD §14)
`enum class EventPriority : uint8_t { Critical=0, High, Normal, Low }` — lower value = more important. **Eviction rule (queue full):** pick the lowest-importance resident; evict it only if strictly lower importance than the incoming event (`evictedLowerPriority++`), else reject the incoming one (`droppedFull++`, `publish` → `Err::Full`). Critical residents are never evicted; if all 16 slots are Critical and a Critical arrives → rejected + counted (pathological storm, visible in stats).

| EventType | Priority | Min repeat (ms) | Why |
|---|---|---|---|
| Boot | **Critical** | 0 (none) | One per boot; anchors the whole timeline (reboot-loop diagnosis, F26) |
| SensorFault | **Critical** | 1000 `[EXPERIMENTAL]` | Hardware loss must survive fault storms; de-dup bounds repeats, first always kept |
| AlertRaised | **Critical** | 0 | The alert ladder record; audit of user-visible behavior |
| BatteryWarning | **Critical** | 60000 `[EXPERIMENTAL]` | Power history matters; battery % drifts slowly → storm-prone |
| SuspectedCataplexy | **Critical** | 0 | Flagship exploratory detection; must never be shed |
| SleepEpisode | High | 0 | Primary history record; transition-only upstream (F08 hysteresis) |
| DrowsinessEvent | High | 0 | Transition-only (F07 hysteresis) |
| UserMarked | High | 0 | Explicit user intent; low volume |
| TimeSync | High | 0 | Companion wall-time reconstruction anchor; rare |
| NapPlanned | Normal | 0 | Transition-only, low volume |
| NapCompleted | Normal | 0 | Transition-only, low volume |
| DeviceNotWorn | Normal | 5000 `[EXPERIMENTAL]` | Don/doff flapping bursts; history needs the fact, not every flip |
| *(Low)* | — | — | Reserved for future chatty types; none of the current 12 |

**§13 answer: critical = {Boot, SensorFault, AlertRaised, BatteryWarning, SuspectedCataplexy}** — never evicted, never rejected while a lower-importance event is queued. Priorities tagged `[DERIVED]` (why column); intervals `[EXPERIMENTAL]` (§8, tune with F28/F29 data).

Counters (`uint32_t`, exposed via `stats()`): `droppedFull`, `evictedLowerPriority`, `rateLimited`, `subscriberRejected`, `appendRetries`, `storeFaultEpisodes`, `queueHighWater`.

### 15.5 wire_eventlog API (what the composition root gets)
```cpp
namespace f17 {
void wire(app::AppContext& ctx);        // build logger (ctx.clock, ctx.faults), publish Event{Boot}, create pump (ARDUINO-only); idempotent, call once BEFORE the attach* calls
bw::IEventSink* sink();                 // P4 passes to f07/f08/f09::attachEventSink(...) and f10::attachOutputs(&f01::led(), &f01::buzzer(), sink())
void attachStore(bw::IEventStore*);     // F18's store: runs boot_id/seq recovery (D3), starts draining; null-safe
bool attachSubscriber(bw::IEventSink*); // fan-out list (F14/F19/F12); false when kMaxEventSubscribers reached
bw::eventlog::EventLogStats stats();    // §15.4 counters (diagnostics/F23 seam)
void pumpOnce(bw::TimeMs now);          // one drain cycle — native test seam
// constants: kEventPumpPeriodMs / kEventPumpStackBytes / kEventPumpPriority / kEventPumpCore
}
```
P4 order: `f01::wire` (clock) → `f17::wire(ctx)` → `f17::attachStore(...)` → `f07/f08/f09::attachEventSink(f17::sink())`, `f10::attachOutputs(..., f17::sink())`, `f17::attachSubscriber(...)`. Order among the attaches is free: F08's pending-FIFO flush and any pre-store events are absorbed by the bounded queue; recovery runs before the first append.

### 15.6 Test list (firmware/test/bw_eventlog/test_bw_eventlog/, Unity, `pio test -e native`)
Fakes owned here: `fake_event_store.h` (persists across logger re-creation; fail-next-N / `Err::Full` mode; append call log; an `AppendHook` that re-enters the logger from inside `append()`; may wrap `bw_testkit::InMemoryEventStore`, which has no fault injection) + `recording_subscriber.h` (`RecordingSubscriber`, reject mode). Reused from bw_testkit: `FakeClock`, `RecordingFaultReporter`.
1. `test_main.cpp` + `fake_event_store.h` + `recording_subscriber.h` — harness/support. **12 files total** (that line plus the 9 `.cpp` below).
2. `test_seq_bootid.cpp` **(§9a)** — empty store → first seq 1, monotonic, in order; simulated reboot (new logger, same store) → seq continues, boot_id 1→2; pre-seeded store (tail seq 500, boot 3) → recovery probe → next event boot 4, seq 501; failed append consumes no seq; store attached late → recovery still correct; store null → queue holds, nothing fanned out.
3. `test_rate_limiter.cpp` **(§9b)** — SensorFault: 2nd within 1000 ms suppressed + counted, at exactly +1000 ms allowed; DeviceNotWorn/BatteryWarning rows; unlimited types never limited; no time reference (no clock, `t_start_ms == 0`) → pass-through, never blocks.
4. `test_overflow_priority.cpp` **(§9b)** — eviction matrix: incoming Critical evicts queued Normal/Low; incoming High vs all-Critical queue → rejected `Err::Full`; equal priority → incoming rejected; Critical resident never evicted; counters + `queueHighWater`; plus the peek/append/evict race: a drain mid-`append()` whose peeked head is evicted must store that event exactly once and orphan nothing (every published event is either stored or still queued).
5. `test_priority_table.cpp` **(DoD §14)** — table-driven: all 12 EventTypes mapped, `static_assert` coverage (no default case left), critical set exactly {Boot, SensorFault, AlertRaised, BatteryWarning, SuspectedCataplexy}; intervals: 0 for non-storm types.
6. `test_wall_time.cpp` **(§9c)** — unix unknown → `t_start_unix = 0` even if the clock syncs before the drain; known → stamped at publish; producer-captured non-zero preserved; Boot emitted once (`detector_id == 17`, schema stamped); TimeSync on 0→known once, second event only on a >5 s jump, plain ticking → none.
7. `test_fanout.cpp` **(D1/D4)** — subscriber receives the appended event with final seq, in order; >8th attach rejected; one subscriber returning `Err::Full` → counted, others still receive, drain continues; zero subscribers still appends.
8. `test_store_failure.cpp` **(§10)** — `Err::Full` → event stays at head, no seq consumed, retry after `kStoreRetryBackoffMs`, `StorageDegraded` reported once per episode (not per retry) via `RecordingFaultReporter`; ordering preserved after recovery; `publish` still `Ok` during the outage; overflow policy applies meanwhile.
9. `test_stamping.cpp` **(D10)** — `t_start_ms` 0→clock filled / producer value kept; `schema_version` always stamped; producer fields untouched; rate-limited → `Ok`, overflow-dropped → `Err::Full`.
10. `test_wire_eventlog.cpp` — native wiring drive: `f17::wire(ctx)` + `pumpOnce` + `attachStore`/`sink()`/`attachSubscriber` seams compile and behave (task body covered by the esp32dev build); repeated `wire()` claims the pump slot exactly once, so idempotence covers the task as well as the Boot.

### 15.7 Budgets, failure behavior, risks
- **RAM:** queue 768 B (§11) + logger/limiter/subscribers/stats ≈ 260 B `[DERIVED]` ≈ **1.0 KB** static; pump stack 3072 B (watermark via `uxTaskGetStackHighWaterMark` + `ESP.getMinFreeHeap()` measured at Validate).
- **CPU:** ≤16 drains per 100 ms tick; critical sections span a 48 B copy only; recovery ≤64 `readFrom` probes, boot only.
- **Failure behavior:** store full/failed → head retained, backoff, one fault report/episode, producers unaffected; no store → bounded queue + priority policy; subscriber rejects → counted, others served; no clock → no wall stamping/TimeSync detection (`t_start_unix = 0`, honest).
- **Risks / `[ASSUMED]`:** (1) D6 flash/core placement of the pump — cross-check against F18's plan at review (top risk; constants localize any fix); (2) `readFrom ≥ seq` probe semantics — F00-documented, pinned by test 2, CR if F18 deviates; (3) a fully empty store restarts `boot_id` at 1 (no persistent boot counter exists in the contracts — best-effort, documented); (4) per-type de-dup can hide a second distinct SensorFault < 1 s (first kept, counters visible; refine at Validate).

---
