# CR-034 — Timestamped qualifying motion for demo wake checks

- Raised by: F06/F10 / 2026-10-19
- Type: additive, in-memory only
- Status: accepted by developer 2026-10-19; applied to contracts, producers,
  forwarding and consumers. Native 1502/1502 and ESP32 build pass; final
  aggregate CI **6/6 PASS**, no skips (F10 status log).

## Problem

The developer requests that qualifying motion restart the demo's 20-second
countdown and clear orange, but never clear latched red. F06 already qualifies
gentle motion above its configurable 0.015 g ENMO threshold. That evidence is
not exposed to F10: Awake also includes quiet contact and a finite evidence hold;
ShortMotionBurst denotes a separate strong-motion/confounder condition.
Neither is an honest replacement for current qualifying motion.

F07 produces snapshots on its own schedule. A reason bit alone, without the
original motion timestamp, could repeatedly restart the timer from cached input.

## Proposal (exact additions)

Add to `docs/interfaces/data_decision.md` and the corresponding bw_core headers:

```cpp
// In-memory F06 evidence, forwarded unchanged by F07. Not an alertness state.
struct WakeMotionEvidence {
    TimeMs t_ms;     // original processed motion-window timestamp
    bool valid;     // usable, fresh, finite motion input; not contact/held evidence
    bool qualifying; // current ENMO strictly exceeds F06's configured demo threshold
};
```

Append `WakeMotionEvidence wake_motion;` to `AlertnessProbabilities` and
`StateSnapshot`. Zero initialization means unavailable; all existing enums and
field meanings remain unchanged. These types are not persisted/transmitted and
have no schema version; Event and BLE layouts remain unchanged.

F06 publishes this independently of epoch/fast-lane probabilities, using the
original motion timestamp. No marker is fabricated during contact grace,
accepted-button evidence or the five-second Awake hold. Invalid/gapped motion
and unsafe wear invalidate evidence. F07 forwards it unchanged, without
refreshing its timestamp or retaining it across safety gates.

F10 consumes only valid, fresh, previously unconsumed qualifying evidence;
never raw MotionFeatures or model probabilities. In normal demo monitoring it
restarts the full interval; in orange it clears/restarts only before the response
deadline; after red latches it cannot clear or reschedule the latch. Safety
gates and deliberate long-clear behavior retain priority. Quiet Awake never
restarts the countdown. The existing strong-burst/stillness path remains separate.

## Alternatives and choice

- Simplest: use Awake, or lower/reinterpret ShortMotionBurst. Rejected: quiet
  contact/held evidence would become movement, or the 0.35 g burst semantics
  would change for other consumers.
- Smaller additive option: one ReasonCode. Rejected: no original timestamp;
  independently ticking F07 snapshots could replay cached movement as new.
- Most scalable: a general versioned multimodal evidence stream. Rejected for
  this tuning: adds a producer/consumer channel and more scheduling machinery.
- Choice: a small timestamped evidence value on the existing F06 -> F07 -> F10
  path. No activity enum change, no extra queues, no feature-header coupling.

## Affected paths and migration

- F00: bw_core evidence type and two appended fields; contract/layout tests.
- F06: populate evidence and tests; preserve gentle/epoch/strong thresholds.
- F07: pass through original evidence and tests (targeted scope extension).
- F10: freshness/deduplication, countdown/response tests; standard policy unchanged.
- F22/F28: no numeric reason mapping or event-format migration needed.
- F12 and the external console: independent presentation work; no sleep episode
  or actual estimator state manufactured from unanswered checks.

Required tests include cached-evidence replay, exact threshold/deadline,
gentle movement, quiet Awake, orange clear, red immunity, and NotWorn/gaps.
