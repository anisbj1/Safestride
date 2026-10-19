# F24 — Operating modes

> Track **E** · Milestone **M5** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Centrally manage operating modes (Day, Night, Nap, Calibration, Research) and the orthogonal power profile; publish a mode profile that other features read (sampling, UI brightness, alert intensity).

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `Command{SetMode, EnterResearch, ExitResearch, StartNap}`
- `IClock` (day/night schedule, if time known)
- `BatteryReading` (LowPower profile)
- Settings (deferred; injected `ModeConfig` defaults in W6, D5)

**Outputs (produced contracts)**
- `OperatingMode`, `PowerProfile` (`LatestValue`)
- `ModeProfile` (config values per mode)
- No mode-change events in W6 (G1 ruling 1, D7); changes are provider-visible only.

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Change behavior directly in other modules (they read the profile).
- Allow implicit transitions.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Modes vs profiles** *(pre-existing, kept)*
- Simplest: One enum incl. Alert and LowPower.
- Professional/scalable: Modes (context) + PowerProfile (orthogonal) + Alert as F10 overlay.
- Trade-offs: Two published values to combine.
- **Choice:** Orthogonal axes (00 §6). Alert is never a mode; LowPower is never a mode.

**D3. Transition matrix and command semantics**
- Problem: §5 forbids implicit transitions; commands must have exactly one legal effect.
- Simplest: any command switches to any mode.
- Scalable: documented matrix; Research is entered/left only via its own commands; self-transitions are no-ops.
- Trade-offs: a couple of rejections to remember — worth it, they prevent losing context (pre-Research mode, research session integrity).
- **Choice:** matrix in §7.1. Rules:
  1. `SetMode(m)`: for non-self transitions, legal only when neither `m` nor `current` is Research. Research entry requires `EnterResearch` so the previous mode is saved; exit requires `ExitResearch`. Rejection ⇒ `Err::InvalidArg`, state unchanged. Self-transitions remain successful no-ops (rule 5).
  2. `StartNap` ≡ `SetMode(Nap)` with reason `NapStart` (button-gesture path; F15 uses `SetMode(Nap)` per docs/05 §4). Illegal in Research.
  3. `EnterResearch`: legal unless already Research; stores `pre_research_mode_`.
  4. `ExitResearch`: legal only from Research; restores `pre_research_mode_` (fallback Day). Illegal elsewhere ⇒ `Err::InvalidArg`.
  5. Self-transition: success, **no event**, mode unchanged (idempotent).
  6. Out-of-enum arg (`arg0` ≥ 5): `Err::InvalidArg`, unchanged.
- Nap is entered only by an explicit command; spontaneous sleep is F08's episode classification, never a mode change (spec §13 resolved).

**D4. Auto day/night scheduler**
- Problem: switch Day↔Night automatically without flapping and without wall time lying.
- Simplest: recompute scheduled mode every tick and switch whenever it differs from current.
- Alternative: switch only on a *window-crossing* event (scheduled mode actually changed since last tick).
- Trade-offs: the simple version undoes an explicit `SetMode(Day)` at 23:00 on the next tick (user-hostile); the crossing version needs one extra byte of state.
- **Choice:** crossing version. `last_scheduled_` tracks the schedule-derived mode; a transition fires only when the schedule itself changes and current ∈ {Day, Night}. Auto never fires in Nap/Calibration/Research. No time known (`unixSeconds()==0`) ⇒ never auto (00 §4: wall time unknown is first-class). Assumption `[ASSUMED]`: phone syncs user-local time; no timezone math on-device.

**D5. Night-hours configurability (§8 retag)**
- Problem: §8 tagged `kNightStartHour/kNightEndHour` `[USER]`, but no production `ISettings` exists yet (empty `SettingKey` table; app.cpp wires `settings = nullptr`, CR-003).
- Simplest: compile-time constants.
- Scalable: values injected via a `ModeConfig` struct from wiring; when an `ISettings` table lands, a later CR appends `SettingKey`s and wiring feeds the values in — `bw_modes` code does not change.
- **Choice:** `ModeConfig{night_start_hour, night_end_hour, low_power_enter_pct, low_power_exit_pct}` with the §8 defaults; retagged `[EXPERIMENTAL]` (honest today) with the `[USER]` intent noted. No SettingKey CR raised now — nothing could consume it.

**D8. Persistence**
- Problem: §10 says "invalid stored mode ⇒ Day"; should mode survive reboot?
- Simplest: no persistence — boot always Day; auto-scheduler restores Night once wall time is known.
- Scalable: persist last mode via `IKeyValueStore` with schema byte + validation.
- **Choice (W6):** no persistence. Keeps RAM/flash minimal; Research/Nap never survive a reboot (safe default); §10 is satisfied trivially (boot = Day) and covers the future persisted path. Revisit only if a consumer shows need.

## 7. Module structure

### 7.1 Legal-transition matrix (DoD §14)
Rows = current mode, cells = legal triggers; anything not listed ⇒ `Err::InvalidArg`, state unchanged. Auto = D4 crossing rule.

| from \ to | Day | Night | Nap | Calibration | Research |
|---|---|---|---|---|---|
| **Day** | no-op | SetMode · Auto | SetMode · StartNap | SetMode | EnterResearch |
| **Night** | SetMode · Auto | no-op | SetMode · StartNap | SetMode | EnterResearch |
| **Nap** | SetMode | SetMode | no-op | SetMode | EnterResearch |
| **Calibration** | SetMode | SetMode | SetMode · StartNap | no-op | EnterResearch |
| **Research** | ExitResearch | ExitResearch | ExitResearch | ExitResearch | no-op |

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kNightStartHour` / `kNightEndHour` | 22 / 7 | `EXPERIMENTAL` (was `USER`; D5 — becomes `[USER]` via a SettingKey CR when `ISettings` exists) | Typical sleep window; injected via `ModeConfig` |
| `kLowPowerEnterPercent` / `kLowPowerExitPercent` | 15 / 25 | `EXPERIMENTAL` | Hysteresis prevents flapping (spec §8) |
| `kLowPowerChargingOverrides` | true | `DERIVED` | Charging ⇒ mains available; low-power restrictions pointless |
| `kModePumpPeriodMs` | 2000 | `DERIVED` | ServicePump's existing cadence (G1 ruling 2); edge-triggered D4 cannot miss a boundary, ≤2 s user-invisible latency |
| `kMaxCommandsPerPump` | 4 | `DERIVED` | Bounded work per tick; leftover commands drain next tick |
| `kModeCommandQueueDepth` | 8 | `EXPERIMENTAL` | Burst of button+BLE commands; small static queue |
| `kDayBrightnessPct` | 100 | `EXPERIMENTAL` | Full brightness day (profile table) |
| `kNightBrightnessPct` | 40 | `EXPERIMENTAL` | Night: readable but non-blinding |
| `kNapBrightnessPct` | 30 | `EXPERIMENTAL` | Wrist at rest near face |
| `kMaintBrightnessPct` | 80 | `EXPERIMENTAL` | Calibration/Research: readable, not full drain |
| `kLowPowerBrightnessCapPct` | 50 | `EXPERIMENTAL` | LowPower caps brightness; alert level never reduced |
| `kDayAlertCap` | Persistent | `EXPERIMENTAL` | Full ladder in day |
| `kNightAlertCap` / `kNapAlertCap` | Active | `EXPERIMENTAL` | Quieter at night/nap; still escalates above warning |
| `kMaintAlertCap` | Warning | `EXPERIMENTAL` | Calibration/Research suppress loud repeated alerts |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan (host, `pio test -e native`; before: 0 — new suite)
Matrix (`test_mode_matrix.cpp`):
1. Every (from,to) pair × `SetMode` mirrors §7.1 (legal ⇒ new mode + ok; illegal ⇒ `Err::InvalidArg` + unchanged) — table-driven, all 25 rows.
2. Self-transition: ok, unchanged, **no** `changed` flag.
3. `EnterResearch` stores/restores pre-mode (from Day, from Nap, from Calibration); repeat `EnterResearch` ⇒ error.
4. `ExitResearch` outside Research ⇒ error, unchanged.
5. `StartNap`: Day→Nap reason `NapStart`; in Research ⇒ error.
6. Out-of-enum `arg0` (e.g. 200) ⇒ `Err::InvalidArg`, unchanged.
Scheduler (`test_auto_schedule.cpp`, FakeClock):
7. 21:59:59 stays Day → 22:00:00 flips Night (event reason AutoSchedule).
8. 06:59:59 Night → 07:00:00 Day; midnight wrap 23:00→00:00 stays Night.
9. `unix_s == 0`: hours cross 22:00, no switch (stays last explicit mode).
10. Auto suppressed in Nap/Calibration/Research across the boundary.
11. Explicit `SetMode(Day)` at 23:00 with known time: stays Day until next crossing (07:00) — the D4 override case.
12. Boot with known time already in night window: first tick Day→Night (initial sync case).
Power (`test_power_policy.cpp`):
13. 15 % ⇒ LowPower; 16 % ⇒ stays Normal; 25 % ⇒ Normal; 24 % ⇒ stays LowPower.
14. `valid==false` ⇒ Normal (even mid-LowPower).
15. `charging_known && charging` ⇒ Normal.
16. Night + LowPower coexist; profile publishes both axes.
Events → transition observation (`test_mode_changes.cpp`; G1 ruling 1: W6 emits no events — the three guarantees are pinned on `ModeOutput`'s `changed`/`reason`, the seam a future event-emitting wiring would read):
17. Exactly one `changed` step per transition, correct `previous_mode` + reason (Command/AutoSchedule/NapStart/ResearchEnter/ResearchExit); settle ticks carry no `changed`.
18. No `changed` on no-op / rejected command / power-only change.
19. Rejected command: state unchanged, never retried, next legal command applies at once (the sink-`Full` case is moot without a sink; queue-full refusal is pinned in the wiring suite).
Publication:
20. `ModeProfile` after each transition: mode, power, brightness/alert values per §8 table, `wall_time_known` flag correct.

## 10. Failure behavior
- Invalid/missing stored mode ⇒ Day (today: boot is always Day, D8).
- Wall time unknown ⇒ no auto switching; last explicit mode stands.
- Battery invalid ⇒ Normal profile; transitions never blocked by sink `Full`.
- Illegal command ⇒ `Err::InvalidArg` to caller, state unchanged, no event.

## 11. Budget
- RAM < 0.5 KB: ModeManager state ≈ 24 B + `ModeProfile` ≈ 24 B + mailbox ≈ 40 B + command queue 8×16 B = ~216 B `[DERIVED]` estimate, not a device measurement. Measure the integrated static-RAM delta at P4; no F24-specific device heap measurement was performed at IMPLEMENT.
- CPU: O(1) per 2 s pump tick (G1 ruling 2), < 0.01 % `[DERIVED]`.

## 12. Out of scope
- Actual sampling changes (owners read the profile).
- SettingKey registration for night hours (deferred until `ISettings` exists, D5).
- Persisting mode across reboot (D8).

## 13. Open questions / assumptions
- ~~Is Nap mode active when the user manually sleeps without a plan?~~ **Resolved (D3):** no — Nap only on `StartNap`/`SetMode(Nap)`; spontaneous sleep stays F08's episode classification. `[ASSUMED]` — flag if the developer disagrees.
- `[ASSUMED]` phone syncs user-local wall time; no timezone handling on-device (00 §4).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Transition matrix documented (§7.1) and mirrored row-by-row by tests (§9.1).
- Auto day/night fires only on window crossing with known wall time (tests 7–12).
- Every transition produces exactly one `changed` step; no-op/reject/power-only produce none (tests 17–19; no events in W6 — G1 ruling 1).
- `ModeProfile` values exist for all (mode, power) combinations (test 20).

---
