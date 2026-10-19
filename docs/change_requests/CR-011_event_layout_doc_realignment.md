# CR-011 - Re-align `data_decision.md` §4.5 Event layout to the accepted CR-002 diff

- Raised by: integrator / 2026-10-04 (found by the cross-feature integration audit)
- Type: correction of an already-accepted change (no new decision; restores CR-002 as accepted)
- Status: **accepted + applied** 2026-10-04 by the integrator (the acceptance authority for
  contract text is the same as CR-001..006; this change *is* CR-002's accepted diff verbatim)

## Problem

CR-002 was accepted 2026-09-30 and recorded as "applied to `docs/interfaces/data_decision.md`
§4.5" on 2026-10-01 (README row, CR-005 split). The application drifted: the §4.5 block in the
locked doc lists a **different field order** than both CR-002's accepted `+` diff and the code:

| | `docs/interfaces/data_decision.md` §4.5 (as found 2026-10-04) | CR-002 accepted diff / `bw/events.h` / `contract_checks.h` |
|---|---|---|
| first field | `uint32_t seq` | `TimeMs t_start_ms` (offset 0) |
| `payload[8]` | last (no offsets given) | offset 32 (8-byte aligned) |
| offsets | none printed | all 16 fields pinned, `static_assert`ed |

Both orders happen to pack to 48 B, so `sizeof`/offset assertions cannot detect the drift —
but a decoder written from the interface doc (F17/F18/F21/F28) would misread every field after
offset 0. `bw/events.h`, `contract_checks.h` and all existing producers (F09 `candidate_event.cpp`)
follow CR-002's accepted order and are correct; **only the doc block is wrong.**

## Change

Replace the §4.5 field-order comment + field list with CR-002's accepted diff (offsets as pinned
in `contract_checks.h`), plus one sentence noting this re-alignment (CR-011). No code, no
`sizeof`, no offsets, no feature behavior changes.

## Affected features

None functionally (code already matches). F17/F18/F21/F28 read §4.5 when they start — this
lands before any of them exists, which is why it is fixed now.
