# F14 — User event confirmation

> Track **C** · Milestone **M3** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
After a detected event, prompt the user to confirm or reject; store the label with the event. The flow must be trivial when drowsy and never penalize no response.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `Event` notifications that require confirmation
- `Command{ConfirmEvent, RejectEvent}`
- `IEventStore.setLabel`
- `IClock`

**Outputs (produced contracts)**
- `ConfirmPromptState` (provider for F12)
- Label update in the store (`Confirmed`/`Rejected`)
- `Event` label change notification

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Render UI.
- Decide what an event is.
- Block alerts waiting for an answer.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Prompt timing**
- Simplest: Prompt immediately.
- Professional/scalable: Prompt after the alert is acknowledged/resolved, queue up to N pending, time out to Unlabeled.
- Trade-offs: Immediate prompts collide with alerts.
- **Choice:** Deferred prompt after ack; small queue; timeout; late labeling possible on the companion (F21).

**D2. Input seam (event / ack / command)**
- Problem: F14 needs event notifications, the alert-ack signal and `ConfirmEvent`/`RejectEvent` commands, but must not include F17/F10 headers (01 §1), and F17/F18 are built in parallel this wave.
- Simplest: call F17's subscriber API directly — rejected: feature→feature include, and F17's fan-out type is not a `bw_core` contract.
- Professional/scalable: plain entry points on `ConfirmationManager` taking `bw_core` value types only, bound by the composition root at wire time (P4):
  `void onEvent(const Event&)`, `void onAlertStatus(const AlertStatus&)`, `bool onCommand(const Command&)`, `void step(TimeMs now)`.
- Trade-off: manual binding in `wire_confirm.cpp` vs. a new contract — binding already exists for every mailbox/queue (01 §3), no CR needed, host tests call the entry points directly.
- **Choice:** entry points. All three input types (`Event`, `AlertStatus`, `Command`) already exist in `bw_core` (`bw/events.h`, `bw/alerts.h`, `bw/commands.h`).

**D5. Label-change notification**
- Problem: spec §3 requires an "Event label change notification" without adding an `EventType` (append-only enum → CR).
- Simplest: reuse existing `EventType::UserMarked` (semantically "user marked"), published via `IEventSink` (F17 appends + fans out).
- Payload mapping (within the existing 8-byte `Event::payload`, no contract change): `payload[0..3]` = labeled `seq` (little-endian), `payload[4]` = `UserLabel`, `payload[5..7]` = 0; `detector_id = 14` (F14's reserved id, §8).
- Guards: F14 ignores events with `detector_id == 14` (no self-trigger), and `UserMarked` is not prompt-eligible (D6) — no feedback loop.
- **Choice:** `UserMarked` reuse; a new `EventType` would need CR-016 and is not justified for one consumer.

**D6. Prompt-eligibility policy**
- Problem: F14 "must not decide what an event is" — yet it must know which events to prompt for.
- Simplest: hard-coded type list inside the manager — rejected: policy hidden in logic, wiring cannot change it.
- Scalable: `ConfirmConfig` carries an eligible-`EventType` set supplied by the wiring; default `{DrowsinessEvent, SuspectedCataplexy}` `[EXPERIMENTAL]`. Non-eligible events are ignored (never queued, never labeled).
- **Choice:** config-owned eligibility set; the *decision* of what deserves a prompt stays configuration/wiring, not F14 logic.

**D7. Store-failure retry (spec §10)**
- Problem: `setLabel` can fail (`Err::Full`, flash stalls) — never block, never lose the label silently.
- Simplest: drop the label on failure — rejected (violates §10). Scalable: bounded RAM retry buffer `kMaxRetryPending` (4 × `{seq,label}` = 32 B), retried lazily on each `step()` up to `kRetryCount` attempts per label, then counted in `label_store_giveups` (label stays `Unlabeled` in the store; F21 can still late-label).
- Choice: bounded retry + give-up counter. No new `FaultCode` (append-only enum → CR-016; not worth expanding its scope).

## 7. Module structure

```
firmware/lib/bw_confirm/
  include/bw/confirm/confirmation_manager.h   // ConfirmationManager: onEvent/onAlertStatus/onCommand/step,
                                              //   promptState()/labelStats(); implements bw::IConfirmPromptProvider (after CR-016)
  include/bw/confirm/confirm_config.h         // ConfirmConfig: eligible-type set + constants (defaults from §8)
  include/bw/confirm/pending_queue.h          // PendingEventQueue: fixed ring {seq, received_ms, type}, drop-oldest
  src/confirmation_manager.cpp
firmware/src/app/wiring/wire_confirm.{h,cpp}  // composition-root adapters: binds F17 fan-out → onEvent,
                                              //   F10 ack → onAlertStatus, command dispatch → onCommand,
                                              //   exports provider to F12. No task planned (see §15).
firmware/test/bw_confirm/test_bw_confirm/     // Unity, layout matches firmware/test/<lib>/test_<lib>/
  main.cpp, confirm_fixtures.h,
  fake_event_store.h    // fake IEventStore: label capture + failure injection (no F17/F18 headers)
  fake_event_sink.h     // fake IEventSink: captures published label-change events
  fake_clock.h          // FakeClock: manual TimeMs advance for scripted timelines
  test_timelines.cpp, test_queue_overflow.cpp, test_store_labels.cpp,
  test_late_label.cpp, test_prompt_state.cpp, test_stats.cpp, test_config_filter.cpp
```

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
Host: `pio test -e native`, suite `bw_confirm`, all fakes local to `firmware/test/bw_confirm/**` (FakeClock timelines; fake `IEventStore` with label capture + failure injection; fake `IEventSink`; no F17/F18 headers).

1. `test_timelines.cpp` — scripted `FakeClock` sequences (spec §9):
   a. event → `onAlertStatus(level, ack=false)` → phase `Deferred`, no prompt; `ack=true` at t → phase `Offered` at t + `kPromptDeferAfterAckMs`; `ConfirmEvent` → `setLabel(seq, Confirmed)` on fake store + `UserMarked` notification captured → `Idle`.
   b. Timeout: `Offered` with no command → at t + `kPromptTimeoutMs` resolves `Unlabeled` (no `setLabel`), `stats.unlabeled++`, next queued event becomes `Offered`.
   c. Alert re-raises while `Offered` → phase back to `Deferred` (prompt never blocks/holds an alert; alert path untouched).
   d. Never-ack: events accumulate, no prompt shown, queue stays bounded, `onEvent` returns immediately.
2. `test_queue_overflow.cpp` — 5 eligible events before any ack → oldest dropped: `UserMarked`+`Unlabeled` marker published for its seq, `stats.dropped_unlabeled==1`, `queue_depth==4`; ack then drains the remaining 4.
3. `test_store_labels.cpp` — labels persist via fake `IEventStore` (§9): `Confirmed`/`Rejected` captured per seq; injected `Err::Full` → retried on next `step()` within `kRetryCount`, `stats.store_retries==1`; always-failing store → give-up after `kRetryCount`, no unbounded loop, manager still responsive.
4. `test_late_label.cpp` — `ConfirmEvent`/`RejectEvent` for a seq not in the queue (timed-out or dropped) → `setLabel` still called (late labeling allowed); unknown-but-valid seq handled the same.
5. `test_prompt_state.cpp` — provider output (CR-016-gated): `Idle→Deferred→Offered→Idle` with correct `seq`, `offer_deadline_ms`, `queue_depth` at each `step`.
6. `test_stats.cpp` — `labelStats()` returns correct confirmed/rejected/unlabeled counts across a mixed timeline (DoD §14).
7. `test_config_filter.cpp` — non-eligible types (`Boot`, `AlertRaised`, `UserMarked`) ignored; events with `detector_id == kDetectorId` ignored (loop guard); wiring-extended eligibility set honored.

## 10. Failure behavior
- Store failure ⇒ keep label in RAM and retry `kRetryCount` times.

## 11. Budget
RAM < 0.5 KB. **Measured (IMPLEMENT, host g++ sizeof + test `test_stats_manager_state_within_ram_budget`, which asserts ≤ 512 B):**
- `sizeof(ConfirmationManager)` = **264 B** — the whole per-instance state (queue 72 B incl. 4×16 B entries, offered entry 16 B, retry buffer 32 B, stats 28 B, provider state 32 B, config + control ≈ 72 B)
- `sizeof(PendingEventQueue)` = 72 B (inside the manager); `sizeof(ConfirmPromptState)` = 32 B (CR-016)
- wire adds 1 instance + 2 pointers (16 B) → ≈ 280 B total < 512 B ✓
- No heap, no new task (§15), CPU O(1) per `onEvent`/`onCommand`, O(kMaxPending ≤ 4) scan per `step()`. esp32dev build: RAM 16.2 %, Flash 28.5 % (whole-firmware figures; F14's share not isolated).

## 12. Out of scope
- Companion review UI (F21).

## 13. Open questions / assumptions
- ~~Exact button mapping (F11 §13).~~ **Resolved 2026-10-12 (P3/F-1):** F11's
  merged gesture table emits `ConfirmEvent`/`RejectEvent` with `arg0 = 0`,
  `arg1 = 0` (pinned by `test/bw_input/test_input_router.cpp:88/98`).
  Convention: `arg0 == 0` means **"the currently offered prompt"** (0 is never
  a valid `Event::seq` — F17 assigns from 1); a non-zero `arg0` is an explicit
  target seq (companion/late labeling, §15). With no prompt offered an
  `arg0 == 0` command is a no-op and never writes seq 0.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Label stats (confirmed/rejected/unlabeled) available to F28 export.
  Accessor: `ConfirmLabelStats labelStats() const` on `ConfirmationManager` —
  `struct ConfirmLabelStats { uint32_t confirmed, rejected, unlabeled, dropped_unlabeled, store_retries, store_giveups, publish_failures; }`.
  F28 gets the persisted side by counting `Event.label` over `IEventStore::readFrom` / exported events (existing contract, labels written via `setLabel` — no new contract needed); the in-RAM accessor serves host tests and diagnostics.

---
## 15. Seams and P4 binding needs (composition root's job)

**Inputs** — plain entry points on `ConfirmationManager` (D2), all `bw_core` value types:
| Seam | Bound at P4 by | Evidence it is needed |
|---|---|---|
| `onEvent(const Event&)` | F17 subscriber fan-out → `wire_confirm` adapter | F17 built in parallel; no subscriber type in `bw_core` today (grep: none) |
| `onAlertStatus(const AlertStatus&)` | F10 ack source → `wire_confirm` adapter (app-owned `LatestValue<AlertStatus>` mailbox pattern) | `AlertStatus` type exists (`bw/alerts.h:15`), **no provider exposes it** (grep `docs/interfaces`: only the struct) — D3 `[ASSUMED]` |
| `onCommand(const Command&)` | command dispatcher routes `ConfirmEvent`/`RejectEvent` here; **`arg0` = target `Event::seq`**, `arg0 == 0` = the offered prompt (F11 button convention, §13), `arg1` = 0 reserved | `bw/commands.h:19-20` exist; no owner routes them yet |

**Outputs:**
| Output | Consumed by | Contract status |
|---|---|---|
| `IConfirmPromptProvider::latest(ConfirmPromptState&)` | F12 (via `wire_confirm` at P4, flips `setPromptAvailable`) | **CR-016 required** (§16) |
| `IEventStore::setLabel(seq, UserLabel)` | F18 | exists (`providers.h:90`) |
| `IEventSink::publish(Event{UserMarked, detector_id=14, payload=seq+label})` | F17 | exists (`providers.h:80`, `events.h:27`) |
| `labelStats()` | host tests, diagnostics, F28 (via store, §14) | own lib, no contract |

Tasks: **none planned** — `step(TimeMs)` is called from an existing app loop. If P4 proves a task mandatory: core 1, priority ≤ 3, period constant declared in `wire_confirm.h` (per brief).

**Title:** Add `ConfirmPromptState` + `IConfirmPromptProvider` to `bw_core`.
**Motivation:** F12's prompt screen (D6 integration point `setPromptAvailable(bool)`, placeholder `bw_ui/src/screens.cpp:113-117`) needs prompt content (seq, phase, deadline, depth) and F12 may only depend on `bw_core` (01 §1 rule 1).
**Proposed diff (append-only; no existing type changes):**
```cpp
// bw/providers.h (or new bw/confirm.h):
enum class ConfirmPromptPhase : uint8_t { Idle, Deferred, Offered };   // append-only, 0 = Idle
struct ConfirmPromptState {                       // trivially copyable, mailbox-safe
  static constexpr uint8_t kSchemaVersion = 1;
  TimeMs t_ms;                // when produced
  uint32_t seq;               // event currently offered; 0 = none
  ConfirmPromptPhase phase;
  TimeMs offer_deadline_ms;   // prompt auto-resolves at this monotonic time; 0 unless Offered
  uint8_t queue_depth;        // pending events behind the offered one
};
class IConfirmPromptProvider { public: virtual bool latest(ConfirmPromptState&) const = 0; };  // false = no state yet
```
**Also in CR-016 scope:** none. `UserMarked` is reused (D5), ack seam is wiring-level (D3), stats ride `IEventStore` (§14).
**Gate:** only the F12 provider output (test 5 + provider methods) waits on acceptance; queue, labeling and stats implement without it.

---
