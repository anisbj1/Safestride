# CR-010 — Carry `ReasonMask reasons` on `AlertnessProbabilities`

- Raised by: F06 / 2026-10-03
- Type: **additive** (new field at the end of an in-memory contract struct; not persisted, not transmitted, no schema version on it)
- Status: **accepted + applied 2026-10-11** (integrator G0 of wave W1): `data_decision.md` §4.4 + `bw/model.h` (`ReasonMask reasons` last). F06 switchover from the `WakeEstimate` adapter is an F06 task; F07 merge policy is F07's call.

## Problem

F06's contract with F07 is `AlertnessProbabilities` (02 §4.4): three probabilities, a
confidence and an `input_quality`. F06 computes far more than that — its heuristic path
scores sustained low motion, HR-below-baseline, time-of-day, abrupt motion drop, and its
model path gets per-feature `contributions()` from `IModel` (02 §4.4) — and it maps both to
`ReasonCode` bits (F06 §3: "contributions become a `ReasonMask` for F22").

There is no field to carry them. Today the mask is only reachable inside F06 (local
adapter `WakeEstimate { AlertnessProbabilities probs; ReasonMask reasons; }`), so:

- **F07 cannot forward evidence it never sees.** Its four observable inputs include this
  struct; `StateSnapshot.reasons` must be justified from what the engine observed
  (CR-008's problem statement), yet the evidence F06 already computed stops at the
  adapter seam.
- **F22 would explain "drowsy" with no cause** whenever F07's own dwell-based codes
  (CR-008) are not the whole story — e.g. the model path's `HrBelowBaseline` /
  `TimeOfDayTypicalDrowsy` contributions that F06 alone knows.
- Dropping the mask at the seam means computing it twice (F06 + F07) or, worse, never —
  00 §1 requires important detections to carry compact reason codes, not invented ones.

## Proposal

Exact diff against `docs/interfaces/data_decision.md` §4.4:

```diff
 struct AlertnessProbabilities { TimeMs t_ms; bool valid; float awake, drowsy, sleep; float confidence;
-     DataQuality input_quality; };
+     DataQuality input_quality; ReasonMask reasons; };
```

- `reasons` is a `ReasonMask` (`uint32_t`, §4.5) — same type as `StateSnapshot.reasons`
  and `Event.reasons`, F22 already decodes it.
- Semantics: "evidence **F06** saw that supports this estimate" — bits set only for
  evidence the estimator actually weighed (00 §1: never fabricate from noise). Empty mask
  = "probabilities only, no reason code claimed".
- Appended **last**, after `input_quality`; the struct is trivially copyable and is
  passed by value through `LatestValue<>`, so no pointer/padding hazard (F00 layout rules).
- F07 decides whether to merge these bits into `StateSnapshot.reasons` alongside its own
  CR-008 codes; it may also ignore them. This CR only widens what F06 can say.

Mirrored change in `firmware/lib/bw_core/include/bw/…` (the header declaring the struct)
is **F00's / the integrator's** to apply — feature implementations never edit `bw_core`.

## Alternatives considered

- **Simplest — keep the local adapter (`WakeEstimate`) inside F06 and stop at the seam.**
  Rejected: it is precisely the status quo this CR exists to change. The mask is then
  unavailable to F07/F22, and every consumer that later needs it must re-derive evidence
  from raw signals it may not own (F07 observes only the four estimate structs).
- **Second simplest — a separate side-channel struct** (`AlertnessEvidence { t_ms;
  ReasonMask; }`) delivered alongside, leaving `AlertnessProbabilities` untouched.
  Rejected: two structs to wire, two to keep timestamp-consistent, and a consumer can
  pair a stale mask with fresh probabilities. One extra `uint32_t` in an existing
  in-memory struct is cheaper than a second synchronization surface.
- **Chosen — append `ReasonMask reasons` to `AlertnessProbabilities`.** One field, one
  timestamp, one type F22 already renders; additive and non-persisted, so no schema
  version bump (02 §4.5's `kSchemaVersion` covers `Event` only).
- **Most scalable — a generic `evidence` block** (per-feature contributions as
  `(feature_index, float, ReasonCode)[]`, enough for F22's "which feature drove it"
  narration). Rejected for now: `IModel::contributions()` output is unbounded in width,
  the struct would stop being trivially copyable unless capped, and F22 has no stated
  need for floats yet — CR-009 records the same reasoning ("`ReasonMask` has spare bits").
  Can be raised later as a follow-up CR if F22 asks for magnitudes; the mask field here
  is forward-compatible with that (the block would be a new type, not a re-type of
  `reasons`).

## Affected features

- **F06** — raiser; deletes its local adapter and emits the field once accepted
  (status log records the switchover).
- **F00 / integrator** — applies the one-line diff to `data_decision.md` §4.4 and the
  mirrored `bw_core` declaration; no layout test pins this struct today (only `Event`
  is size-pinned), but adding `sizeof` pinning is optional and welcome.
- **F07** — first potential consumer: decides merge policy into `StateSnapshot.reasons`
  (independent of CR-008, no ordering dependency).
- **F22** — gains the text mapping for bits F06 sets; no code change beyond what
  CR-008/CR-009 already require (`ReasonMask` decode is type-generic).
- **F05, F09, F17, F18** — none: `FeatureVector`, `ModelOutput` and stored `Event`
  are unchanged; `AlertnessProbabilities` is neither persisted nor transmitted.

## Migration / compatibility

Additive: trailing field on an in-memory struct, all consumers compiled from the same
tree, so there is no version skew on device. Callers that ignore `reasons` behave exactly
as before; F06 initializes it to `0` when no evidence is claimed. Until F00 applies this,
F06 keeps the local adapter (`WakeEstimate`) — no feature is blocked, and the switchover
is a two-line change in `wire_wake.cpp` recorded in F06's status log.

## Status

proposed — awaiting review (F06 §6 D3 raises this CR; F06 §14 adds its acceptance to the
Definition of Done).
