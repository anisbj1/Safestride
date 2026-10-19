# CR-018 — `BleLinkState` + `IBleLinkProvider` (F19 connection-state output)

- **Raised by:** F19 (wave 4 plan, 2026-10-15), drafted in `docs/features/F19_ble_communication.md` §16
- **Type:** additive (integrator authority, `05` §6)
- **Status:** **accepted + applied 2026-10-15 (integrator, W4 G1)** to `firmware/lib/bw_core/include/bw/providers.h` + `docs/interfaces/services.md` §5

## Problem
F19's spec §3 mandates a "connection state provider (for F12/F23)" output, and
`HealthSnapshot.ble_connected` (consumed by F12's diagnostics screen, produced by
F23) currently has **no readable source**: `wire_diag.cpp` hardcodes `false`
(`[ASSUMED] F19 is W4: nothing connected`). No link/connection contract exists in
`bw_core`, and building one inside `bw_ble` would violate `01` §1 (features
depend on `bw_core` only — F23/F12 must not include F19 headers). Rejected
alternatives: piggybacking on `HealthSnapshot` (F23 produces it; F19 must not
depend on F23's header), a raw `bool` getter in wiring (no timestamp, no
encryption state, untestable seam).

## Proposal — additive to `bw/providers.h` and `docs/interfaces/services.md` §5

```cpp
// CR-018 (accepted 2026-10-15): BLE link state, F19 produces / F12+F23 consume.
// In-memory only, never persisted or transmitted -> no kSchemaVersion (core.md §1).
struct BleLinkState {
    TimeMs t_ms;      // when produced
    bool connected;   // >= 1 GAP link up
    bool encrypted;   // link encrypted & bonded (F19 D1: Just Works)
    uint16_t mtu;     // negotiated ATT MTU; 23 (kDefaultAttMtu) before negotiation
};
class IBleLinkProvider {
public:
    virtual bool latest(BleLinkState&) const = 0;  // false = no snapshot yet
};
```

## Migration / compatibility
Purely additive (new struct + new provider interface in `bw_core`); no existing
type, enum value or signature changes; features that ignore it are unaffected.
Trivially copyable, publishable through a `LatestValue` mailbox like the other
providers.

## Affected features
- **F19** implements it (`wire_ble` getter consumed at integration).
- **F23** (full, W6) fills `HealthSnapshot.ble_connected` from it — until then the
  honest `false` default stays, and the `[ASSUMED]` comments are flipped at that
  switch-over. **F12** may render from it via `HealthSnapshot` only.
- **F26** may supervise `BleDown` from it later (F19 already reports `FaultCode::BleDown`).

## Test plan
`bw_ble` host suite pins production semantics (snapshot only while a link exists,
`mtu` = 0/23/255 transitions); the services doc ↔ header parity style used by
existing contract tests covers the declarations.
