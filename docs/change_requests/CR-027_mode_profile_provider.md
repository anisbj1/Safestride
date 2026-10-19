# CR-027 — Add `ModeProfile` + `IModeProvider` to `bw_core`

- **Date:** 2026-10-18 · **Wave:** W6 G1 · **Raised by:** F24 (PLAN, W6) · **Decided by:** integrator
- **Type:** additive (new header + umbrella include; no existing struct/enum touched)
- **Status: accepted + applied 2026-10-18** (applied on `master`, G1; commit in this wave's history)

## Problem
F24 produces an operating-mode profile that other features must read (sampling in F01,
alert intensity in F10, brightness in F12). Consumers may depend on `bw_core` only
(`01_ARCHITECTURE.md` §1 rule 1), so the profile cannot live in `bw_modes`. No
`bw_core` vocabulary for it exists today (`ModeProfile`/`IModeProvider` absent —
verified by grep at G0).

## Proposal
New header `firmware/lib/bw_core/include/bw/mode_profile.h`:

```cpp
struct ModeProfile {
    static constexpr uint8_t kSchemaVersion = 1;
    TimeMs t_ms;                 // when produced (monotonic; 0 = never produced)
    OperatingMode mode;          // bw/state.h
    PowerProfile power;          // bw/state.h — orthogonal to mode (00 §6)
    uint8_t ui_brightness_pct;   // 0..100 hint for F12
    AlertLevel alert_max_level;  // cap for the F10 ladder (02 §4.6)
    bool wall_time_known;        // true = auto day/night scheduling is active
};
class IModeProvider { public: virtual bool latest(ModeProfile&) const = 0; };  // false = none yet
```

Plus `#include <bw/mode_profile.h>` in the `bw/core.h` umbrella (alphabetical slot)
and the `services.md` §5 rows.

## Alternatives
- (a) Consumers read `StateSnapshot.mode/power` (already in `bw/state.h`) — rejected:
  `StateSnapshot` is F07's alertness/wear snapshot; its `mode` field is a copy with no
  profile hints (brightness, alert cap), and F07 would then have to depend on F24.
- (b) Feature-local provider in `bw_modes` — rejected: every consumer would include a
  feature header (01 §1 r1 violation).
- (c) This CR — accepted: value-type contract in `bw_core`, matching the CR-015/016/018
  provider pattern exactly.

## Trade-offs
Adds a small permanent contract surface that F01/F10/F12 have not yet consumed; a
field added later is another append. Accepted: the shape is the one F24's transition
matrix needs, and unused fields are hints with honest defaults, not dead coupling.

## Migration / affected
Purely additive; no schema or wire impact (in-memory only). Producer: F24 (W6).
Consumers bind later at their own touch; nothing breaks if no one reads it.
