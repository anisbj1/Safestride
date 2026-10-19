# F10 — Alert engine

> Track **C** · Milestone **M2** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Independent alert system that turns states/events/commands into alert levels (Normal → Warning → Active → Persistent) and non-blocking output patterns for LED, buzzer and OLED banner, with cooldowns, persistence, acknowledgment and hysteresis.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `StateSnapshot`
- Event notifications (`Event` of relevant types via wiring)
- `Command{AcknowledgeAlert, CancelAlert}`
- `OperatingMode` intensity profile (from snapshot)

**Outputs (produced contracts)**
- `AlertStatus` (`LatestValue`)
- Actuator calls on `IRgbLed`, `IBuzzer` (via `PatternPlayer`)
- Banner request for F12 through `AlertStatus`
- `Event{AlertRaised}` via `IEventSink`

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Read ML output or sensors.
- Block (patterns are time-sliced).
- Depend on BLE/phone — phone notification is a consumer of `AlertStatus`/events elsewhere.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Escalation logic**
- Simplest: Alert whenever state is Drowsy.
- Professional/scalable: Level machine with persistence, cooldown and acknowledgment memory.
- Trade-offs: More states, but stops noisy repeats and respects acknowledgment.
- **Choice:** Level machine (below); rules table in the spec.

**D2. Output driving**
- Simplest: Direct `delay()`-based beeps.
- Professional/scalable: `PatternPlayer` stepping pattern tables against time.
- Trade-offs: Player needs a small table format; keeps AlertTask responsive.
- **Choice:** `PatternPlayer` with pattern IDs; patterns defined in `AlertPatterns` config.

**D3. Escalation ladder (proposal)**
- Simplest: Warning: LED amber + OLED banner, silent.
- Professional/scalable: Active: LED + short buzzer beeps; Persistent: louder/longer, repeats until ack or max duration.
- Trade-offs: Persistent must not run forever (battery, annoyance).
- **Choice:** Persistent auto-stops after `kPersistentMaxMs` and logs it.

### 6.1 Rule table (one `evaluate()` call, stages in this order, first match wins)

| Stage | Condition | Demand | Why / reason in `AlertStatus.reasons` |
|---|---|---|---|
| R1 | no snapshot yet, or newest snapshot older than `kStateStaleMs` | Normal | gate closed; `SensorUnavailable` |
| R2 | `wear == NotWorn` **or** `alertness == NotWorn` | Normal | gate closed; `DeviceNotWorn` (wear and alertness are independent axes, 00 §6) |
| R3 | `alertness == Unknown` | Normal | gate closed; F07's own `reasons` say why |
| R4 | worn + `Awake` | Normal | open; snapshot `reasons` |
| R5 | worn + `Drowsy` | Warning | open; snapshot `reasons` |
| R6 | worn + `HighDrowsinessRisk` | Active | open; snapshot `reasons` |
| R7 | worn + `PossibleSleep` | Persistent | open; snapshot `reasons` |
| — | `wear == Uncertain` is **not** closed | conservative: R5/R6/R7 still apply | an unsure wear must not silence a real cue |

Then, in order, before the ladder:

1. **Event floor** (D4) — only while the gate is open; raises the demand, never lowers it.
2. **Commands** (D5) — `CancelAlert`/`AcknowledgeAlert` outrank every rule and stop immediately.
3. **Input lost** (D6) — beyond `kStateStaleMs` ⇒ Normal now, `StateStale`, **no cooldown**.
4. **Persistent max duration** (D3) — ≥ `kPersistentMaxMs` ⇒ Normal now, `AutoStoppedMaxDuration`, cooldown starts.
5. **Suppression** (D5) — ack snooze blocks demands ≤ the acked level; cooldown blocks all new demand.
6. **Sustained Warning** — Warning demand held ≥ `kWarningToActiveMs` ⇒ demand becomes Active (beeps). The timer measures the demand at its **current value**: it restarts whenever the demand value changes (the policy tracks the previous demand), so only *continuous* Warning demand — the very value this stage acts on — can escalate; time spent at a higher demand (e.g. Active) never counts towards it, and an Active ⇒ Warning step-down made by stage 8 stays visible for a full `kWarningToActiveMs`.
7. **Mode ceiling** (D7) — demand capped at `ModeIntensity::max_level` for the current `OperatingMode`.
8. **Ladder** (D1) — up immediately, down only after `kDeescalateHoldMs`; at most one transition (and one event) per call.

**One-tick input contract (commands and notifications).** `Command{AcknowledgeAlert}` / `Command{CancelAlert}` and event notifications are presented for **exactly one** `evaluate()` tick. The policy does not edge-detect them, so holding a level across ticks re-applies it every tick: a `CancelAlert` held for three consecutive ticks pushes `cooldown_until_` forward three times (test `test_policy_cancel_held_across_ticks_pushes_cooldown_each_tick`). *Choice:* document the pulse contract instead of latch-and-clear inside `evaluate()` — **(a)** simplest option: no extra "previous input" member, `evaluate()` stays a pure function of its input; **(b)** scalable option: latch the first edge and ignore the rest until the input is released. Trade-offs: (b) is robust against a sloppy producer but hides edge memory in the policy and changes behavior for every producer, while (a) costs at most one tick (50 ms at 20 Hz) per held tick and is met by construction because commands arrive from a drained queue (one delivery = one tick). **Choice: (a)** — the contract is stated here, in `alert_policy.h`, and enforced by the pump.

### 6.2 Ladder diagram (must match the tests, §14)

```
        raise: demand > level → IMMEDIATE (stage 8), unless gated / suppressed / mode-capped

  Normal ──R5──▶ Warning ──R6──▶ Active ──R7──▶ Persistent
     ▲             │               │                 │
     │             └── demand lower than level: held kDeescalateHoldMs, then step down (phase
     │                 Resolved on the way to Normal, Raised between levels)
     │
     ├─ AcknowledgeAlert ......... Normal NOW, then demand ≤ acked level ignored for kAckSnoozeMs
     │                             (a HIGHER level still raises at once - ack never blocks escalation)
     ├─ CancelAlert .............. Normal NOW, then all new demand ignored for kCooldownMs
     ├─ snapshot stale > kStateStaleMs . Normal NOW (StateStale, no cooldown)
     └─ Persistent ≥ kPersistentMaxMs ... Normal NOW (AutoStoppedMaxDuration) + kCooldownMs

  Warning demand held ≥ kWarningToActiveMs ⇒ demand steps to Active  (tests:
      warning_escalates_after_persistence, warning_level_sustained_escalates_back,
      demand_change_restarts_the_sustained_timer - the timer restarts whenever the
      demand VALUE changes, so stage 6 measures continuous Warning demand only)
  Raising from Normal ⇒ Acknowledged=false, Persistent clock restarts on entering
      Persistent (test: persistent_clock_restarts_on_reentry).
```

A lower demand steps **directly to the new level in one transition** (one `AlertRaised` event): the ladder never walks down one level per call — `Persistent ⇒ Warning` lands in a single step, exactly like an upward jump (stage 8 writes `demand` in one `changeTo()`; test `test_policy_deescalation_between_levels_reports_raised`).

**D4. Event input (spec gap: events were listed as input but never mapped)**
- Simplest: ignore events; alert on state only.
- Professional/scalable: per-event floor levels held briefly, gated exactly like state.
- Trade-offs: needs a small mapping table; an ungated floor could buzz with the device off the wrist.
- **Choice:** `SuspectedCataplexy → Active`, `DrowsinessEvent → Warning` (both worded "suspected"/"possible" upstream, 00 §1) held for `kEventAlertHoldMs`, applied only when the gate is open; floors raise, never lower; other event types floor nothing.
  - A floor-raised alert **inherits the state snapshot's reasons**: the floor changes only the level, never the explanation, and no reason is fabricated for the event — the payload level (`payload[1]`, §6 D8) is the only cause marker an `AlertRaised` for a floor carries.
  - A notification received while the gate is **closed** is held, not dropped: it may still fire when the gate reopens, provided that happens within `kEventAlertHoldMs` of the notification (the hold clock runs from the notification, not from the gate opening; test `test_policy_event_floor_held_while_gated_fires_after_reopen`).

**D5. Acknowledge vs cancel**
- Simplest: one "silence" button with one timer.
- Professional/scalable: two distinct suppression memories (short same-level snooze vs full cooldown).
- Trade-offs: two commands, two windows; but a user's "I'm doing it now" must not equal "stop for 10 minutes".
- **Choice:** Ack = stop now + same-level snooze for `kAckSnoozeMs` (higher levels always escalate); Cancel = stop now + `kCooldownMs` over all demand, and it clears the ack memory.

**D6. Stale reading**
- Simplest: keep the last level forever.
- Professional/scalable: a staleness window that doubles as the hold.
- Trade-offs: a dead pipeline must not leave a lit alert, but a slow one must not flap.
- **Choice:** `kStateStaleMs` *is* the hold: hold exactly the window, then Normal + `SensorUnavailable` + `StateStale`, quality `Insufficient`, confidence `0.0f`, and **no cooldown** (the condition may be unchanged, so a fresh snapshot must re-raise immediately).

**D7. Operating-mode intensity profile**
- Simplest: one global brightness/volume.
- Professional/scalable: a per-mode table `{max_level, led_gain_pct, buzzer_enabled}`.
- Trade-offs: a table invites per-mode policy questions the spec does not answer yet (F24 night, F15 nap).
- **Choice:** per-mode `ModeIntensity` table, all ceilings = `Persistent` for now (the spec's Inputs line says mode supplies an *intensity* profile: intensity only). `max_level` is the extension point — one row change caps the ladder with no engine change; unknown mode ⇒ Day fallback.

**D8. `AlertRaised` payload (02 §4.5 gives 8 type-specific bytes; F10 must define them)**
- Simplest: empty payload, level only in the record type.
- Professional/scalable: explicit fixed layout, reserved tail zeroed.
- Trade-offs: layout must be documented or decoders guess.
- **Choice:** `payload[0] = AlertPhase` (`Raised/Resolved/Acknowledged/Cancelled/AutoStoppedMaxDuration/StateStale`), `payload[1] = AlertLevel`, `payload[2..7]` reserved = 0; `detector_id = kAlertFeatureId` (10); `t_start_unix = 0` (no wall clock); `seq`/`boot_id` stay 0 — F17 assigns `seq`, and **no producer of `boot_id` exists yet** (see §13).

## 7. Module structure (files)

### D10. End-to-end motion age is not the consecutive-evidence gap (2026-10-19)
- Problem: F06 diagnostic capture confirms 1992 ms-old otherwise usable motion.
  The old F10 1500 ms age check additionally ignores normal F07 forwarding;
  comparing each source stamp to the previous reset's receipt time also skips
  subsequent new delayed windows, violating every-movement restart semantics.
- Simplest: globally widen `kDemoEvidenceGapMs`. Rejected: would silently permit
  missing stillness updates to bridge the burst/stillness trigger.
- Scalable: event-driven runtime with stage deadline contracts. Deferred: needs
  integration changes outside the targeted F06/F10 repair.
- Choice: separate `kDemoMotionFreshnessMs=3550` [DERIVED]: window-END to
  F02 publication 1000 + F06 consumption 1000 + F07 forwarding 1000 + F10 tick
  50 + scheduling slack 500 ms. Keep continuity at 1500 ms. Original timestamps
  are never restamped. Use a distinct monitoring-session start to reject
  pre-gate evidence; deduplicate by source timestamp, not last reset receipt.
  Every new qualifying window restarts the normal full 20 s interval, even if
  its source stamp predates the previous reset receipt. Orange still requires
  movement observed after its onset and before its deadline; red ignores it.
  Standard ladder, mode caps, removal gates and sleep episode logic unchanged.

### D9. Opt-in academic demo wake-check protocol (2026-10-19)
- Problem: state-driven suppression timers cannot express periodic checks while
  already Awake, nor a red alert that requires a deliberate long response.
- Simplest: shorten the standard ladder timers. Rejected: cooldown/snooze still
  defeat the cadence and motion would resolve red.
- Scalable: a separate configurable protocol state machine sharing the policy's
  snapshots, status, patterns and events. Choice: a small `evaluateDemo` branch,
  enabled only by `AlertConfig::demo_wake_protocol` in `wire_alert.cpp`; the
  default standard policy and its tests remain unchanged.
- While fresh Worn + known alertness + usable IMU: after **20000 ms**, orange
  Warning with the existing two short onset beeps and OLED banner. Fresh
  qualifying motion restarts the full interval (developer tuning, CR-034).
  A late tick starts one check, never a burst of missed checks.
- Orange: Big Short or **fresh qualifying motion** clears the check, publishes
  a passed response to F06/F07, and restarts the interval. F06 supplies CR-034
  evidence above its experimental **0.015 g ENMO** threshold; F10 checks the
  original timestamp and consumes each window once. Quiet Awake/contact and
  cached snapshots cannot restart the timer. The **0.35 g** strong-burst lane
  remains separate for the burst-followed-by-stillness trigger.
  Button timing uses the command's actual release timestamp, so an on-time
  release queued just before the deadline survives task quantisation; old,
  future and exact-deadline releases are rejected.
- At **5000 ms** unanswered Warning, latch Active red with the existing longer
  repeating beeps. Deadline beats simultaneous short/motion; Big Long (unchanged
  **800 ms**, on release) wins even at the deadline. Once red, short/motion cannot
  clear it. There is deliberately **no demo auto-stop/cooldown/snooze**: red
  requires CancelAlert or a safety gate. Standard Persistent max remains intact.
- Fresh `ShortMotionBurst + SustainedLowMotion` begins a stillness run; **8000 ms**
  of contiguous low-motion snapshots asks orange FIRST, then the same 5 s window.
  Movement, an evidence gap > **1500 ms**, or any safety gate breaks the run.
  The dwell is on producer timestamps, not repeated reads of one snapshot.
- NotWorn, Uncertain wear, Unknown alertness, missing/stale state or unavailable
  IMU stops immediately and resets scheduling. Rewear starts a new full interval.
  Gate stops never publish a passed check. Mode ceilings still cap outputs;
  the underlying red latch survives a lower ceiling until explicitly cancelled.
- F10 is the sole device producer of `wake_check_passed_ms`: `PolicyOutput` and
  `AlertOutput` carry a local accepted-response flag; wiring only forwards it.
  No shared contract changed. F10 still reads no sensors or model output.
- This is an experimental possible-drowsiness overlay, not a sleep diagnosis.
  No F08 episode threshold or F07 PossibleSleep dwell is shortened. Existing
  Pattern tables are retained unchanged. F12's opt-in demo presentation is a
  cleared screen with centred blinking Drowsy? / Sleeping? prompts; these do
  not rename estimator states or produce sleep episodes.

Demo constants in `alert_config.h`: `kDemoCheckIntervalMs=20000`,
`kDemoResponseWindowMs=5000`, `kDemoBurstStillnessMs=8000` [EXPERIMENTAL];
`kDemoEvidenceGapMs=1500` [DERIVED: one 1 Hz window + scheduling slack].
`kDemoMotionFreshnessMs=3550` [DERIVED: end-to-end age, D10; not continuity].
`kDemoTelemetryPeriodMs=1000` [DERIVED: one log per evidence-window period,
plus immediate level changes]. `[WAKECHECK]` exposes device timestamps and
absolute next-check/response deadlines; no guessed desktop countdown.

| File | Contents |
|---|---|
| `lib/bw_alert/include/bw/alert/alert_config.h` | §8 constants, `AlertConfig`, `ModeIntensity`, `AlertPhase`, `reasonBit` |
| `lib/bw_alert/include/bw/alert/alert_patterns.h` | `AlertPatterns`: pattern tables + per-level LED/buzzer assignment |
| `lib/bw_alert/include/bw/alert/alert_policy.h` + `src/alert_policy.cpp` | `AlertPolicy`: §6.1 rules, ladder, ack/cooldown memory, `AlertRaised` build |
| `lib/bw_alert/include/bw/alert/pattern_player.h` + `src/pattern_player.cpp` | `PatternPlayer`: time-sliced stepping, change-only actuator writes |
| `lib/bw_alert/include/bw/alert/alert_engine.h` + `src/alert_engine.cpp` | `AlertEngine::step` = policy + player + one event per transition, late DI |
| `src/app/wiring/wire_alert.h/.cpp` | construction, `engine()`, `attachOutputs()`; the 20 Hz pump/task is deferred (§13) |
| `test/bw_alert/test_bw_alert/*` | Unity suite: `test_alert_policy`, `test_pattern_player`, `test_alert_engine`, fixtures, recording fakes |

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kWarningToActiveMs` | 30000 | `EXPERIMENTAL` | Time in Warning conditions before beeping |
| `kAckSnoozeMs` | 300000 | `USER` | After acknowledgment, suppress same-level alert for 5 min |
| `kCooldownMs` | 600000 | `EXPERIMENTAL` | Prevents nagging after a resolved alert |
| `kPersistentMaxMs` | 120000 | `EXPERIMENTAL` | Battery and annoyance bound |
| `kBuzzerFreqHz` | 2700 | `EXPERIMENTAL` | Typical resonance of small piezo/passive buzzers; ignored for active buzzers |
| `kStateStaleMs` | 10000 | `EXPERIMENTAL` | Staleness window (§10); StateTask runs at 1 s, so this tolerates late updates without false de-escalation |
| `kDeescalateHoldMs` | 10000 | `EXPERIMENTAL` | Hysteresis (D1): rejects one-window state flaps |
| `kEventAlertHoldMs` | 30000 | `EXPERIMENTAL` | How long an event notification (D4) floors the demand |
| `kAlertTickPeriodMs` | 50 | `DERIVED` | AlertTask 20 Hz (§11); also the pattern-step quantisation bound |
| `kMaxDurationFieldMs` | 0xFFFFFFFF | `HW` | `duration_ms`/`since_ms` are uint32 (02 §4.5/§4.6): saturate, never wrap |
| `kLedFullScale` | 255 | `HW` | Rgb channels are uint8 full-scale PWM (02 §3) |
| `kPercentFull` | 100 | `DERIVED` | Denominator for `led_gain_pct` (00 §7.5) |
| `kAlertFeatureId` | 10 | `DERIVED` | `Event.detector_id` for `AlertRaised` (02 §4.5) |
| `kAlertPayloadPhaseIndex` / `kAlertPayloadLevelIndex` | 0 / 1 | `DERIVED` | D8 payload layout; `kAlertPayloadReservedBytes` = 6 stay 0 |
| `kModeIntensityCount` | 5 | `DERIVED` | One `ModeIntensity` row per `OperatingMode` (02 §4.5), unknown ⇒ Day |
| patterns (§ D2) | see `alert_patterns.h` | `EXPERIMENTAL` | Warning = 1 Hz blink, silent; Active = beep burst; Persistent = denser burst (D3) |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Sequence tests with `FakeClock` for the ladder, ack, cancel, cooldown, snooze.
- Pattern player timing accuracy against `RecordingLed/Buzzer`.
- No alert while `NotWorn`/`Unknown` unless the rule table explicitly says so.

Covered on the host (`pio test -e native -f "*bw_alert*"`, Unity, no sleeps, no hardware):
- `test_alert_policy.cpp` — §6.1 row by row, the §6.2 ladder timeline (raise, `kWarningToActiveMs`, `kDeescalateHoldMs` incl. the Active ⇒ Warning step-down and the sustained-demand timer restart, `kStateStaleMs` incl. a 32-bit boundary), ack/snooze/never-blocks-higher, cancel/cooldown incl. the one-tick push-forward, `kPersistentMaxMs` auto-stop + clock restart on re-entry, event floor raise/expiry/gating incl. a floor held across a closed gate, between-level drop phase, 32-bit saturation of `since_ms`/`duration_ms`, mode ceiling + unknown-mode fallback, reasons/since/acknowledged status, size budgets.
- `test_pattern_player.cpp` — pattern↔level map, per-level timing against `RecordingLed/Buzzer`, intensity scaling, buzzer-disabled profile, null actuators, time jump / backwards clock safety, size budget.
- `test_alert_engine.cpp` — one `AlertRaised` per transition, ack event + status flag, sink absent ⇒ `NotReady`, sink error surfaced (never ignored), night dimming, pattern follows level, stale stop, size budget.

## 10. Failure behavior
- Missing state updates ⇒ hold current level for `kStateStaleMs`, then de-escalate to Normal and log `SensorUnavailable` (`AlertPhase::StateStale`, quality `Insufficient`, confidence 0). **No cooldown** afterwards: the condition may be unchanged, so a fresh snapshot must re-raise immediately.
- Persistent running too long ⇒ auto-stop at `kPersistentMaxMs` with `AlertPhase::AutoStoppedMaxDuration`, then cooldown.
- No sink/actuators attached yet ⇒ levels and events still decide; `AlertOutput::publish_err = NotReady`, reported by the pump through `IFaultReporter`.

## 11. Budget
RAM < 1 KB; AlertTask 20 Hz. Asserted on host: `sizeof(AlertPolicy) < 512`, `sizeof(AlertConfig) < 256`, `sizeof(AlertEngine) < 1024`, `sizeof(PatternPlayer) < 128`.

## 12. Out of scope
- Phone notifications.
- Snooze UI (F11/F12).

## 13. Open questions / assumptions
- Persistent-alert behavior at night (F24 profile).
- Planned-nap wake alert priority (F15).
- Assumption (D7): the mode profile is intensity-only for now — every mode keeps the full ladder; `ModeIntensity::max_level` is the one-line extension point once F24/F15 answer.
- Assumption (D5): cooldown suppresses *all* new demand, ack snooze only demands ≤ the acked level.
- `Event.boot_id` (02 §4.5) has no producer anywhere yet; F10 writes 0. Needs an owner (likely boot/uptime bookkeeping) or a documented "0 = unknown".
- The 20 Hz pump / AlertTask and the snapshot-command plumbing are **deferred**: `AppContext` carries no StateSnapshot mailbox, command queue, event channel or actuator/sink yet (F00 D9), so `wire_alert` exposes `engine()` + `attachOutputs()` and the composition root starts the pump when those exist. `AlertOutput::publish_err` is to be reported through `ctx.faults`.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Ladder diagram in the spec matches tests.

---
