# CR-009 — Append confounder `ReasonCode` values so F09 can log them as reasons

- Raised by: F09 / 2026-10-03
- Type: **additive** (append-only enum addition; no field, no renumbering)
- Status: **accepted + applied 2026-10-11** (integrator G0 of wave W1): bits 15–17, appended **after** CR-008's values per the G0 ordering decision; `data_decision.md` §4.5 + `bw/state.h`; pinned in `test_contract_layout.cpp`. F09 switch-over from the `payload[1]` adapter is an F09 task.

## Problem

F09's design decision **D2** (spec §6) chose: *"Log confounder flags in reason codes; evaluate
false-alert rate per day (F29)."* The locked contract has nowhere to put them:

- `ReasonCode` (`docs/interfaces/data_decision.md` §4.5, code in `bw_core/include/bw/state.h`)
  ends at `DeviceNotWorn` and contains no confounder value — `AbruptMotionDrop` names the
  trigger, not the objection to it.
- The enum is **append-only, never renumbered** (02 §6.6), and `bw_core` headers are owned by
  F00, so F09 may not add the values itself.
- F09 therefore cannot satisfy its own spec without either editing a locked contract (forbidden)
  or inventing a second, feature-private channel for information that F22 must render, F18/F17
  must store and F28/F29 must analyse.

## Proposal (exact diff)

In `docs/interfaces/data_decision.md` §4.5:

```diff
 enum class ReasonCode : uint8_t { SustainedLowMotion, ProlongedInactivity, HrBelowBaseline, HrTrendChange,
      AbruptMotionDrop, TimeOfDayTypicalDrowsy, MatchesPastConfirmedEvent, UserMarked, PlannedNap,
-     LowSignalQuality, SensorUnavailable, DeviceNotWorn, /* … append only, never renumber */ };
+     LowSignalQuality, SensorUnavailable, DeviceNotWorn,
+     PossibleFall, PostureChange, ShortMotionBurst, /* … append only, never renumber */ };
```

In `firmware/lib/bw_core/include/bw/state.h` (F00 applies, same order):

```diff
     LowSignalQuality,
     SensorUnavailable,
     DeviceNotWorn,
+    PossibleFall,      // F09 §6 D2: descent looked uncontrolled (jerk/impact spike)
+    PostureChange,     // F09 §6 D2: wrist re-oriented across the drop (sitting/laying down)
+    ShortMotionBurst,  // F09 §6 D2: activity lasted less than the look-back (gesture)
     // … append only, never renumber (02 §6.6)
 };
```

Meanings (for F22's text table and F28's export):

| Name | Means |
|---|---|
| `PossibleFall` | the drop was preceded by an uncontrolled descent; confidence was penalised |
| `PostureChange` | the wrist rotated ≥ `kPostureTiltDeltaDeg` across the drop; confidence was penalised |
| `ShortMotionBurst` | the preceding activity was shorter than the look-back; confidence was penalised |

**Ordering:** this CR appends *after whatever the last value is at acceptance time* — the
three names are independent of their numeric value, so no number is pinned here. Two other
proposals append to the same `ReasonCode` block on the same day and both are additive:
CR-008 (F07, state-engine justification, which documents its own values as 12-14). Whoever
lands second resolves the merge and updates this table; nothing here depends on the ordering.
(CR-007 does not touch `ReasonCode` — it adds an `EpisodePhase` enum — so only CR-008 is a
merge competitor for this block.)

These are **diagnostics on a suspected event**, never a claim that a fall or a posture change
occurred — wording stays "possible"/"suspected" (00 §1).

## Alternatives considered (simplest vs scalable) and why rejected

- **Simplest — keep the flags in `Event::payload[1]` (F09-local):** zero contract churn, works
  today. Rejected as the *final* answer: only 8 payload bytes exist and byte 0 already carries
  the raw-window request; F22 could not render the objection next to the reason text, F28/F29
  would need to decode a feature-private byte, and every consumer would have to know F09's
  payload layout — that is exactly the sideways coupling 01 §1 forbids. **F09 keeps using this as
  a local adapter until the CR is decided.**
- **Most scalable — a shared `ConfounderMask` contract type in `bw_core` (versioned struct):**
  reusable by any detector, independent of `ReasonMask` width. Rejected for now: exactly one
  feature produces confounders today, so it buys a new versioned contract and a second parallel
  flag channel for a single consumer; it can still be added later if a second detector needs
  confounders that are *not* reasons.
- **Choice — append three `ReasonCode` values:** the contract already declares the append-only
  mechanism, the codes ride every path that already carries reasons (store, sync, render,
  export) with no new plumbing, and `ReasonMask` has 20 spare bits. Cheapest option that makes
  D2 true for all consumers rather than for F09 alone.

## Affected features

- **F09** — raises the flags (currently via the payload adapter; switches to `ReasonMask` on
  acceptance), updates spec §6 D2/D4 and §8 and its status log.
- **F00** — appends the three enumerators to `bw_core`'s `bw/state.h` (owner of the header).
- **F22** — owns the human-readable text for the three new codes.
- **F17 / F18** — no code change (they store `ReasonMask` opaquely), but stored records will
  start carrying the three new bits.
- **F28 / F29** — the export/analysis columns for reasons gain three values.

## Migration / compatibility notes

- Append-only: existing numeric values keep their meaning; no persisted record changes
  interpretation, records written before the change simply have the bits clear.
- `ReasonMask` is `uint32_t`; 12 existing + 3 (CR-008) + 3 (this CR) = 18 < 32, so no width
  change and no schema bump for `Event` (`kSchemaVersion` stays 1; the mask's content is
  already variable) regardless of which of CR-007/008/009 lands first.
- F09's switch-over: OR the three bits into `CandidateOutput::reasons` and stop writing
  `Event::payload[1]` (bytes 2–7 remain reserved zero). The F09 tests
  (`test_candidate_detector.cpp`, `test_candidate_event.cpp`) are updated in the same session.
- Until then F09 ships with the payload adapter and `ConfounderMask`; behaviour is unchanged.

## Status

**proposed** — 2026-10-03, by F09. Awaiting owner/integrator decision (contract-change guidance §3.4:
F09 continues against the local adapter meanwhile).

This file is CR-009 because CR-007 (F08) and CR-008 (F07) were taken first — the folder was
re-checked for the next free number immediately before creating it. The row in
`docs/change_requests/README.md` is left to the integrator: F09 does not own that file.
