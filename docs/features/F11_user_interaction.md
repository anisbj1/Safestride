# F11 — User interaction (buttons)

> Track **C** · Milestone **M2** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Turn raw button states into gestures and then into context-aware `Command`s. Must stay operable during severe drowsiness: few, forgiving, consistent actions.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `IButtonInput` raw levels
- Current input context (from `AlertStatus`/UI prompt state via provider)

**Outputs (produced contracts)**
- `ButtonEvent` (single-button gestures; the chord is not representable — D9)
- `Command` via `ICommandSink`

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Know what commands do.
- Access GPIO.
- Use blocking debounce.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Gesture set**
- Simplest: Short/long only.
- Professional/scalable: Short/long/double + chord for rare actions.
- Trade-offs: Double/chord are harder when drowsy.
- **Choice:** Short and long only for frequent actions; chord (both buttons 3 s) only for pairing/reset.

**D2. Context routing**
- Simplest: Fixed mapping.
- Professional/scalable: Mapping table per context (Idle, AlertActive, ConfirmPrompt, Menu).
- Trade-offs: Table needs docs but avoids surprises.
- **Choice:** Table (below).

### D2-N — Normative per-context mapping table (**FINAL**, 2026-10-11; DoD §14: tests must match this table cell by cell)

Contexts (`InputContextId`): `Idle`, `AlertActive`, `ConfirmPrompt`, `Menu`.
Gestures: Big short, Big long, Small short, Small long, Chord (both buttons down together ≥ `kChordHoldMs`).

| Gesture | Idle | AlertActive | ConfirmPrompt | Menu |
|---|---|---|---|---|
| Big short | `OpenMenu` **[ASSUMED]** | `AcknowledgeAlert` | `ConfirmEvent` | no command |
| Big long | no command | `CancelAlert` | no command | no command |
| Small short | `NextScreen` `arg0=kStepOnce` | `NextScreen` `arg0=kStepOnce` | `RejectEvent` | `NextScreen` `arg0=kStepOnce` (scroll rule) |
| Small long | `MarkEvent` | no command | no command | no command |
| Chord (both ≥ 3 s) | `StartPairing` | `StartPairing` | `StartPairing` | `StartPairing` |

**§13 conflict resolved ('mark event' vs 'reject'):** `MarkEvent` and `RejectEvent` are never reachable in the same context — reject exists only as `Small short` in `ConfirmPrompt`, mark only as `Small long` in `Idle`; while a prompt is pending the context *is* `ConfirmPrompt`, so `Small long` is a deliberate no-op. One-line rationale: D2 makes meaning context-bound, so a pending confirmation can never be mislabelled by a marking gesture.

**D3. Command emission point (when a gesture produces its Command)**
- Simplest: fire at the threshold while still held (feedback ~0.8 s earlier).
- Professional/scalable: fire on release — Short = released with duration < `kLongPressMs`, Long = released with duration ≥ `kLongPressMs`, Chord = the later button is released after both were down together ≥ `kChordHoldMs`.
- Trade-offs: threshold-based gives earlier feedback but a stuck press (§10) would already have emitted its Long at 0.8 s, so the 10 s "ignore" rule could not be airtight; release-based costs one release delay (users release immediately after the threshold) and makes §10 exact: a stuck press never emits anything.
- **Choice:** release-based for all three gesture kinds.

**D4. Context derivation (wave 1)**
- Simplest: routing context = fixed Idle.
- Professional/scalable: a pure `deriveContext(...)` in `bw_input` fed by wiring each cycle.
- Trade-offs: deriving needs a source for `ConfirmPrompt`/`Menu` (F12/F14, not yet integrated).
- **Choice:** wave 1 derives `Idle`/`AlertActive` from `bw::AlertStatus` (`level == Normal` → Idle, else AlertActive; mailbox empty → Idle). `ConfirmPrompt`/`Menu` are defined, tabled and host-tested now; their sources arrive later as an additive AppContext field — no contract change, no CR.

**D5. Stuck button (> 10 s)**
- Continuous debounced-down ≥ `kButtonStuckMs` → suppress that button's gestures until its next release, emit exactly **one** fault edge per stuck episode: `IFaultReporter::report(FaultCode::ButtonStuck, FaultSeverity::Warning, t_ms)` (severity Warning: input capability degraded, sensing unaffected). Re-arm on release; never report once per 100 Hz sample. Buttons held from boot run stuck detection normally (the boot-ignore only suppresses gestures, not the hardware truth).

**D6. Engine I/O and the ICommandSink seam**
- Simplest: `InputEngine` holds an `IButtonInput*` and pushes commands itself.
- Professional/scalable: pure `step(t_ms, context, big_raw, small_raw) -> StepOutput` with `RoutedGesture{gesture, button_event?, command?}` + `stuck[2]`. The task reads `IButtonInput` and pushes via `ICommandSink`. The `QueueCommandSink` adapter over any `SpscQueue<Command,N>` (the `ctx.commands` backing type) ships **in the library** (`bw/input/queue_command_sink.h`, header-only, pure bw_core glue) rather than in wiring, so the drop path is host-testable (§9); wiring only instantiates it. `push()==false` = full → drop, counted by the queue's drop counter, never blocks, never crashes.
- Trade-offs: passing raw bools duplicates the `IButtonInput` read in the task (one line); holding the interface would drag a hardware dependency into every host test. Adapter-in-library adds one header vs. wiring-local; bought host coverage of the full-queue path — accepted.
- **Choice:** pure `step` (bw_core only, no GPIO, no blocking — §5); adapter in `bw_input`.

**D7. Scroll-rule split (owner-accepted rule, §13)**
- Problem: the rule needs "how many options does this screen have", which F11 must not know (§12).
- Alternatives: (a) F11 keeps option counts in its context → violates §12/layering; (b) CR a new `ScrollOption` CommandType → new contract for no gain; (c) F11 always emits `NextScreen arg0=kStepOnce`, F12 resolves one step against its screen model.
- **Choice:** (c) — no new contract, rule implemented where the screen model lives; documented here so F12's tests and this table agree.

**D9. Chord vs. `bw::ButtonEvent` (contract limitation, no CR)**
- Problem: `bw::ButtonGesture` has only Short/Long/Double; there is no Chord value, and appending one is a locked-contract change (`02` §6.6). No consumer reads button events today, so a CR would spend a contract change for nothing.
- Choice: F11 keeps an internal `GestureKey` (incl. `Chord`) as the routing vocabulary, converts single-button gestures 1:1 into `bw::ButtonEvent` via `toButtonEvent()`, and reports the chord as "not representable" (`false`, output untouched). The chord still produces its `StartPairing` command. If a future consumer needs chord `ButtonEvent`s, raise a CR to append `ButtonGesture::Chord` — noted, not blocking.

## 7. Module structure
Library `firmware/lib/bw_input/` — `bw_core` only, no GPIO, no blocking debounce (§5), host-testable:

| File | Contents |
|---|---|
| `include/bw/input/input_constants.h` | every named value of §8 that the library compiles (`kDebounceMs`, `kLongPressMs`, `kChordHoldMs`, `kButtonStuckMs`, `kChordReleaseMs`, `kStepOnce`, `kCommandSourceButton`) — single source of truth, spec↔code (H9) |
| `include/bw/input/input_types.h` + `src/input_types.cpp` | `InputContextId`, `GestureKey` (BigShort, BigLong, SmallShort, SmallLong, Chord), `struct InputContext`, `buttonIndex()`, `deriveContext(...)`, `GestureRecord`, `toButtonEvent(...)` (D9) |
| `include/bw/input/button_debouncer.h` + `src/button_debouncer.cpp` | `ButtonDebouncer`: raw level → stable pressed state per button, driven by `step(t_ms, button, raw)`; time-based window (rewind-safe), no blocking debounce |
| `include/bw/input/gesture_detector.h` + `src/gesture_detector.cpp` | `GestureDetector`: debounced levels → release-based `GestureRecord`s (D3), chord candidate/resolve/abort logic, `kChordReleaseMs` window, boot-ignore-until-first-release, stuck detection (D5) |
| `include/bw/input/input_router.h` + `src/input_router.cpp` | `InputRouter` + `kInputMap`: static `constexpr` 20-cell table = D2-N (incl. explicit "no command" cells with a `kNoCommandType` sentinel); `route(context, gesture) -> has_command + Command` |
| `include/bw/input/queue_command_sink.h` | header-only `QueueCommandSink<N>`: `ICommandSink` over `SpscQueue<Command,N>` (D6), host-testable drop path |
| `include/bw/input/input_engine.h` + `src/input_engine.cpp` | `InputEngine`: debouncer → detector → router in one `step()`, `StepOutput{RoutedGesture[], stuck[2]}`, `reset()`; the only object the task drives |
| `firmware/src/app/wiring/wire_input.h/.cpp` (namespace `f11`) | construction + DI, reads `f01::buttons()` (composition-level bind), derives context from `ctx.alert_status`, pushes via `QueueCommandSink` over `ctx.commands`, reports stuck via `ctx.faults`, creates `InputTask` (100 Hz, §11) — no logic (wiring README); P4 seam per D8 |

The bw_input library never includes another feature's headers; `bw_hal`'s `GpioButtons` is reached only inside `wire_input` at composition level, through F01's `f01::buttons()` accessor (`bw::IButtonInput&`, sanctioned by `wire_hal.h`; `f01::wire` runs first per the README call order).

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kDebounceMs` | 20 | `EXPERIMENTAL` | Typical tactile-switch bounce is 5–20 ms |
| `kLongPressMs` | 800 | `USER` | Long enough to avoid accidental triggers, short enough for a drowsy user |
| `kDoublePressWindowMs` | 350 | `EXPERIMENTAL` | Comfortable double-press interval. **Reserved — not compiled in v1**: detecting doubles would delay *every* short press by the whole window (D1 excludes doubles from frequent actions); kept here so the tuning value is not rediscovered, activates only via a confirmed table change |
| `kChordHoldMs` | 3000 | `EXPERIMENTAL` | Deliberate two-button hold |
| `kButtonStuckMs` | 10000 | `DERIVED` | §10 and the `FaultCode::ButtonStuck` contract comment ("pressed > 10 s") pin the threshold; long enough to ride out sleeve/pocket pressure, short enough to free a jammed button |
| `kChordReleaseMs` | 500 | `EXPERIMENTAL` | After a chord is emitted, suppress new gestures for this window: the two releases of a chord land milliseconds apart, and their rebound must not register as a fresh ShortPress that immediately acts on the next screen |
| `kStepOnce` | 1 | `DERIVED` | `arg0` of `NextScreen` = "advance exactly one step" (D7); 1 leaves 0 free for a future absolute/no-op meaning |
| `kCommandSourceButton` | 0 | `ASSUMED` | `Command.source` byte for buttons, from the `Button \| Ble \| Internal` comment order in `bw/commands.h`; pinned by constant + test until a CR pins the enum in `bw_core` |
| `kInputTaskPeriodMs` | 10 | `DERIVED` | §11 says InputTask 100 Hz, `runtime.md` §3.2 says 10 ms — same value; one sample per 10 ms gives `kDebounceMs` a 2-sample resolution |
| `kInputTaskStackBytes` | 2560 | `EXPERIMENTAL` | Task holds no buffers or deep call chains; measure the high-water mark at H3 and right-size |
| `kInputTaskPriority` | 3 | `DERIVED` | Med, in `wire_input.h`: strictly below SensorTask (5) and ProcessingTask (4) — the sensor path has hard FIFO/queue budgets it can miss, button input has none to lose — and equal to the F03 pump (3), both µs-scale periodic tasks. Responsiveness *and* stuck detection survive that rank: per-tick work is µs-scale and every rule is timestamp-based (D3/D5), so a late tick shifts latency (bounded by the 20/800 ms windows), never correctness — a button held across a starvation gap crosses `kButtonStuckMs` and reports at the first tick that runs |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).
Library constants live in `bw/input/input_constants.h`; task constants (period, stack, priority, core 1) live in `wire_input.*`, never inline in `app.cpp`.

## 9. Test plan
Host suite `firmware/test/bw_input/test_bw_input/` (`pio test -e native`), Unity, deterministic timelines via explicit `t_ms` (10 ms ticks = the InputTask period; 1 ms ticks for boundary tests) — no wall clock, no sleeps. **Implemented 2026-10-11: 66 cases across 6 test files** (target was ≥ 55); no test may be removed or loosened later (G2).

- `main.cpp` — Unity entry; every test file exposes one `register_*` entry point that `RUN_TEST`s everything it defines.
- `fixture_helpers.h` — `EngineFeed`: scripted raw timeline over `InputEngine` (tick/cycleBig/cycleSmall/cycleBoth) that collects events and stuck edges; test-only, no device code.
- `test_debouncer.cpp` (8): clean press/release; stability exactly `kDebounceMs` vs one sample less; 200 ms bounce burst → no state change; chatter (flip per tick) never toggles; buttons independent; `reset()`; clock rewind restarts the window.
- `test_gesture_detector.cpp` (16): Short emitted on release; Long at exactly `kLongPressMs` **and not** while held (D3); boundaries 799/801 ms (1 ms ticks); 5 s hold silent until release; chord → exactly one event on the later release; chord aborted below `kChordHoldMs` → no output, no stray singles; recovery after abort; `kChordReleaseMs` suppresses the re-press (incl. the exact boundary press *at* the window edge, which is not suppressed); **button held during boot ignored until first release** (gestures only — a boot-held button still reaches `kButtonStuckMs` → one stuck edge); stuck: one edge, no gesture escapes, re-arm, second episode reports again; no chord against a boot-ignored partner; time gap still measures Long.
- `test_input_router.cpp` (22): **one test per cell of the D2-N table (20 cells)** asserting exact `CommandType`/`arg0`/`arg1`/`source`/`t_ms` or "no command" (out-`Command` untouched), plus table completeness (20 cells, every context×gesture exactly once) — the table in this spec and the table in the test are the same 20 rows (DoD §14).
- `test_context.cpp` (8): `deriveContext` — Normal → Idle, Warning/Active/Persistent → AlertActive, empty mailbox → Idle; `InputContext` value pass-through; `toButtonEvent` 1:1 for the four singles + chord → `false` (D9).
- `test_engine.cpp` (8): raw timeline → exact `Command` envelope with `t_ms` = debounced release time; full queue (`push()==false`) → drop counted, pop → next push works, null-sink refuses (D6); `TimeMs` beyond 32 bits; `reset()` clears an interrupted chord attempt; bounce-then-hold → one gesture; stuck end-to-end + re-arm; boot-held end-to-end; no heap over 1000 mixed steps (`operator new` counters, bw_ml pattern).
- `test_contract.cpp` (4): §8 constants match the spec table; `FaultCode::ButtonStuck` = 10 and `ButtonId` order pinned; `static_assert`s (trivially-copyable value types, table size 20).

## 10. Failure behavior
- Stuck button > 10 s ⇒ ignore and report `FaultCode` via `IFaultReporter`.

## 11. Budget
- **RAM:** < 0.5 KB (§11) for the library; expected ≈ 100–200 B (engine + 2× debouncer/gesture state). `InputMap` is `const` → flash, not RAM. The 16-entry `ctx.commands` queue lives in AppContext and is not counted here.
- **CPU:** one O(1) `step()` every 10 ms ≈ negligible (< 0.5 % of core 1).
- **Task:** `InputTask`, period `kInputTaskPeriodMs` = 10 ms (100 Hz), core 1, priority `kInputTaskPriority` = 3 (see §8), stack `kInputTaskStackBytes` = 2560.
- **Measurement (at IMPLEMENT/Validate):** `uxTaskGetStackHighWaterMark` for the stack, `ESP.getMinFreeHeap()` before/after wiring for RAM; numbers recorded in the status log and checked at H2/H3.

## 12. Out of scope
- Menu contents (F12).
- Touch/gesture sensing via IMU.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Mapping table published in the spec and identical to the tests.

---
