# F15 — Scheduled nap management

> Track **C** · Milestone **M5** · Runs on **firmware + companion** · Contract changes only via change request.

## 1. Responsibility
Plan naps (start time, target duration, reminder, wake alert) and compare planned vs. detected sleep. Distinguish planned naps from automatically detected sleep.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `Command{StartNap, CancelNap, SetSetting}` (plan via config)
- `IClock` (needs `unixSeconds`; unknown ⇒ only relative naps)
- Sleep-episode events (F08) to compare
- `IKeyValueStore`

**Outputs (produced contracts)**
- `NapPhase` (Planned, Reminder, InNap, WakeAlert, Complete)
- `Command{SetMode(Nap)}` request to F24
- Wake alert request to F10 (as an `Event{NapCompleted}` / dedicated signal in `AlertStatus` inputs)
- `Event{NapPlanned, NapCompleted}`

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Detect sleep itself.
- Assume wall time is known.
- Make medical recommendations about nap length.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Time reference**
- Simplest: Absolute schedule only.
- Professional/scalable: Absolute schedule when wall time known; 'nap now for N minutes' otherwise.
- Trade-offs: Two modes to test.
- **Choice:** Support both; relative nap works with no phone.

## 7. Module structure
- `NapScheduler`
- `NapPlan` (versioned, persisted)
- `NapComparison`
- companion: nap planning screen + repository

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kDefaultNapMs` | 1200000 | `USER` | 20 min default: a common short-nap length; user-configurable |
| `kReminderLeadMs` | 300000 | `USER` | 5 min reminder |
| `kWakeGraceMs` | 60000 | `EXPERIMENTAL` | Tolerance before declaring nap overrun |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Timeline tests with `FakeClock`; relative and absolute plans; cancel; reboot during nap (plan persistence).
- Comparison of planned vs detected episodes.

## 10. Failure behavior
- Corrupt plan ⇒ discard and log.

## 11. Budget
RAM < 1 KB; flash ~100 B per plan.

## 12. Out of scope
- Recurring/calendar integration.

## 13. Open questions / assumptions

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Companion side compiles/tests per DEC-005 choice.

---
