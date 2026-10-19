# CR-008 — Append three `ReasonCode` values for state-engine justification

- Raised by: F07 / 2026-10-03
- Type: **additive** (append-only enum, 02 §6.6; no field, layout or schema change)
- Status: **accepted + applied 2026-10-11** (integrator G0 of wave W1): bits 12–14 in `data_decision.md` §4.5 + `bw/state.h`; pinned in `test_contract_layout.cpp`. Landed before CR-009 (G0 ordering decision).

## Problem

`StateSnapshot.reasons` and `Event.reasons` are a `ReasonMask` — a bitset of `ReasonCode`
(02 §4.5) whose meaning is "why the current alertness state / event was chosen" (F22 owns
the text, the codes live in `bw_core`). F07 is required to document and test its reason
codes (F07 §14) and 00 §5 requires important detections to carry compact reason codes.

F07 can observe exactly four things: `AlertnessProbabilities`, `WearEstimate`,
`ActivityEstimate` and `SourceQuality` (plus mode/power). For three of its four outcomes
there is **no code it can honestly set**:

| F07 outcome | Closest existing code | Why it does not fit |
|---|---|---|
| `Drowsy` / `HighDrowsinessRisk` / `PossibleSleep` (the primary claim) | `SustainedLowMotion`, `ProlongedInactivity`, `HrBelowBaseline`, `AbruptMotionDrop` | F07 never sees motion or HR — it sees model probabilities. Setting one of these would be a **fabricated justification** (00 §1). |
| `TimeOfDayTypicalDrowsy` | — | Needs wall time + baseline (F13); both may be unknown, and F07 does not read them. |
| `Awake` (recovery) | none | "calm evidence sustained" is unrepresented. |
| `Unknown` because inputs were valid but nothing was sustained | `SensorUnavailable`, `LowSignalQuality` | Both blame the sensors, which are healthy in this case — F07's design deliberately keeps "healthy sensors, inconclusive evidence" apart from sensor failure (F07 D8/E3, §10). |

Setting **no** code is not a real option: an empty mask on a `DrowsinessEvent` gives F22
and F12 nothing to explain, and the test in F07 §14 ("reason codes documented and covered
by tests") would be vacuous.

## Proposal

Append three values at the **end** of `bw::ReasonCode` (current last value:
`DeviceNotWorn` = 11 → new values 12, 13, 14; nothing renumbered).

Diff against `docs/interfaces/data_decision.md` §4.5 (and the mirrored block in
`docs/interfaces/core.md` if the index points there):

```diff
 enum class ReasonCode : uint8_t { SustainedLowMotion, ProlongedInactivity, HrBelowBaseline, HrTrendChange,
      AbruptMotionDrop, TimeOfDayTypicalDrowsy, MatchesPastConfirmedEvent, UserMarked, PlannedNap,
-     LowSignalQuality, SensorUnavailable, DeviceNotWorn, /* … append only, never renumber */ };
+     LowSignalQuality, SensorUnavailable, DeviceNotWorn,
+     SustainedLowAlertnessProbability,  // smoothed alertness probabilities held above the
+                                        // state's enter threshold for its minimum duration (F07 D8/E1)
+     SustainedWakefulnessProbability,   // calm evidence held for the recovery dwell (F07 D8/E3)
+     InsufficientEvidence,              // inputs valid but no state was sustained (F07 D8/E3)
+     /* … append only, never renumber */ };
```

Mirrored change in `firmware/lib/bw_core/include/bw/state.h` (F00's file — **F00 or the
integrator applies it**, feature implementations never edit `bw_core`).

Bit positions: `1u << 12`, `1u << 13`, `1u << 14`.

## Alternatives considered

- **Simplest — leave the mask empty for probabilistic states.** Rejected: 00 §5 makes
  reason codes mandatory for important detections; F22/F12 would render "why?" as nothing
  for the one state that matters most.
- **Reuse `ProlongedInactivity` / `TimeOfDayTypicalDrowsy` / `HrBelowBaseline`.** Rejected:
  F07 cannot observe motion, HR or time of day — the code would assert evidence the
  engine never had (00 §1 "never fabricate").
- **Most scalable — a dedicated `evidence` struct on `StateSnapshot`** (per-state
  probabilities, dwell achieved, thresholds used). Rejected for now: `StateSnapshot` is
  persisted and transmitted, so a new field is a breaking change with a schema bump and
  touches F17/F18/F19/F28; and the `ReasonMask` already exists for exactly this purpose.
  Can be raised later if F22 needs numeric evidence rather than a code.
- **One code instead of two** (single `SustainedLowAlertnessProbability` used for
  `Awake` too). Rejected: the code would then mean both "drowsy evidence" and "wake
  evidence", forcing F22 to infer the meaning from the state — a second, less explicit
  contract. Two codes keep each bit self-describing; the cost of the third code
  (`InsufficientEvidence`) is the same as the second.

## Affected features

- **F07** — raise/first consumer; implements D9 (gate bits + these codes).
- **F22** — owns the human-readable text for the three new codes.
- **F00 / integrator** — applies the header change and extends
  `test_bw_core/test_bw_core/test_contract_layout.cpp` (it pins values 0…11 today;
  appending does not break it, pinning the new values is optional but recommended).
- **F17, F18, F28** — none: `ReasonMask` is a `uint32_t` on the wire and in flash; no
  layout, size or schema change (`Event.kSchemaVersion` stays 1).

## Migration / compatibility

Additive and append-only: existing masks decode unchanged, old records keep their
meaning, no renumbering (02 §6.6). Records written before the change simply never carry
bits 12–14. No feature may be blocked meanwhile: F07 D9 defines the fallback (gate bits
only) and the status log records which path was taken.

**Note:** CR-007 (F08, `EpisodePhase`/`EpisodeStatus`) targets the same §4.5 block and
was raised the same day. Both are additive and independent — no ordering dependency —
but whoever lands second resolves the merge. This file is CR-008 because CR-007 was
taken first; the `README.md` index row is left to the integrator (not F07-owned).

## Status

proposed — awaiting review (F07 spec §13 lists this CR; F07 §14 adds its acceptance to
the Definition of Done).
