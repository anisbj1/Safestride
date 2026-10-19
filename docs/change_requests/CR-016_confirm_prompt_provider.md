# CR-016 — Confirm prompt contract (`ConfirmPromptState` + `IConfirmPromptProvider`)

- **Raised by:** F14 (plan stage, wave 3), 2026-10-12 — integrator files.
- **Status:** **accepted + applied** 2026-10-12 (integrator, W3 G1; additive, integrator authority per docs/05 §6).
- **Applies to:** `docs/interfaces/services.md` §5, `firmware/lib/bw_core/include/bw/providers.h` (append-only).

## Motivation

F12's prompt screen (integration point `setPromptAvailable(bool)`, placeholder in
`bw_ui/src/screens.cpp`) needs prompt content (seq, phase, deadline, queue depth), and
F12 may only depend on `bw_core` (01 §1 rule 1). No contract today carries confirmation
prompt state; the type must live in `bw_core` for F12 to consume it without seeing
`bw_confirm` headers.

## Change (append-only; no existing type modified)

```cpp
// bw/providers.h — F14 produces, F12 consumes.
enum class ConfirmPromptPhase : uint8_t { Idle, Deferred, Offered };  // append-only, 0 = Idle

// Trivially copyable, mailbox-safe.
struct ConfirmPromptState {
    static constexpr uint8_t kSchemaVersion = 1;
    TimeMs t_ms;                 // when produced
    uint32_t seq;                // event currently offered; 0 = none
    ConfirmPromptPhase phase;
    TimeMs offer_deadline_ms;    // prompt auto-resolves at this monotonic time; 0 unless Offered
    uint8_t queue_depth;         // pending events behind the offered one
};
class IConfirmPromptProvider {
public:
    virtual bool latest(ConfirmPromptState&) const = 0;  // false = no state yet
};
```

Out of CR scope: `UserMarked` is reused for label-change notifications (F14 D5), the
alert-ack seam is wiring-level (F14 D3), and label stats ride `IEventStore`.
