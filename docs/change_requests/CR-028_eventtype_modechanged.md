# CR-028 — Append `EventType::ModeChanged`

- **Date:** 2026-10-18 · **Wave:** W6 G1 · **Raised by:** F24 (PLAN, W6) · **Decided by:** integrator
- **Type:** append-only enum (`bw/events.h`, after `Boot`, value 12)
- **Status: REJECTED for W6 — re-raise in a wave that contains F18 (+ F16/F22)**

## Proposal (as raised)
Append `EventType::ModeChanged`; F24 emits one event per mode transition
(detector_id 24, confidence 1.0, payload `[0]` from, `[1]` to, `[2]` reason,
`[3]` power, `[4..7]` reserved zero). Persisted and synced like every event.

## Why rejected for W6
An `EventType` append is contract-cheap but **integration-expensive**, and three of
the affected features are not in this wave:

- `firmware/lib/bw_storage/src/record_codec.cpp` validates every persisted event with
  `isKnownType(uint8_t)` — F18's code, F18's `Owns`, and F18 has no contributor in W6.
  Without that edit the new event is *rejected at the log seam* (encode refuses
  unknown types by design, `test_encode_rejects_unknown_type_and_oversize`).
- Downstream mappings that classify event types (`bw_history` day rules, F22's explain
  catalog, the companion repository / tools export tables) would each need a look.

Editing absent features' `Owns` paths from W6 would violate the one-writer rule
(`docs/00_PROJECT_CONTEXT.md`), and a partial application would ship an event type that cannot be
persisted — worse than not emitting it.

## Alternatives
- (a) Accept and edit F18/F16/F22 from this wave — rejected (rule above).
- (b) Emit the transition under an existing type (e.g. `UserMarked`) — rejected:
  dishonest labeling; the enum means what its name says.
- (c) Accept the rejection: F24 publishes the mode through `IModeProvider` (CR-027);
  mode changes are observable in RAM and on the UI, and the event lands later —
  **chosen**.

## Re-raise condition
A wave that contains F18 (storage codec) — W7 (time/ML track) is the natural home if
F18's owner is in it; otherwise attach to the next F18 touch. The number CR-028 is
**burned, not reused** (CR-014/017/020/023-025 precedent).

## Effect on F24 (W6)
No events, no detector_id consumption; detector_id **24 stays reserved** for the
future append. The transition matrix, provider publication and tests are unaffected.
