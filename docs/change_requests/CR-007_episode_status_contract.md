# CR-007 — Add `EpisodePhase` / `EpisodeStatus` to the §4.5 state contracts (F08 live status)

- Raised by: F08 / 2026-10-03
- Type: additive
- Status: **accepted + applied 2026-10-11** (integrator G0 of wave W1): `data_decision.md` §4.5 + `bw/state.h` (`EpisodePhase`, `EpisodeClass`, `EpisodeStatus`).

## Problem

F08's spec §3 requires "Episode status for UI (`Idle/Candidate/Active`) as `LatestValue`", but no
episode-status type exists in `bw_core`. F08 cannot supply one itself: 01 §1 rule 1 forbids a feature
from including another feature's headers, so `bw_sleep` may not be included by F11/F12 (or by the
companion/BLE side, which only sees `bw_core` types).

`LatestValue<T>` is a container: it still needs a `T` that both producer and consumer can name. The
precedent is `AlertStatus` (F10) — the struct lives in `bw_core` (`bw/alerts.h`), the mailbox instance
is owned by the composition root. F08 has no equivalent struct to point at.

## Proposal — exact diff

Diff target is `docs/interfaces/data_decision.md` §4.5 ("State and events"), the file §4.5 lives in
since CR-005; the same block mirrors into `docs/02_INTERFACES.md` when the integrator synces it
(same handling as CR-002/CR-003).

```diff
 ### 4.5 State and events
 ```cpp
 enum class AlertnessState : uint8_t { Unknown, NotWorn, Awake, Drowsy, HighDrowsinessRisk, PossibleSleep };
 enum class OperatingMode : uint8_t { Day, Night, Nap, Calibration, Research };
+
+// F08 produces, F11/F12 consume (mailbox: app-owned LatestValue<EpisodeStatus>).
+// Append-only: never renumber or reorder (02 §6.6). Not persisted or transmitted,
+// so no kSchemaVersion (02 §1).
+enum class EpisodePhase : uint8_t { Idle, Candidate, Active };
+enum class EpisodeClass : uint8_t { Uncertain, Rest, Nap, MainSleep };  // Uncertain == 0 on purpose
+struct EpisodeStatus {
+    TimeMs t_ms;             // when this status was produced
+    TimeMs t_start_ms;       // monotonic start of the current/last episode; 0 when Idle
+    EpisodePhase phase;
+    EpisodeClass episode_class;  // provisional while phase != Idle; final value is in the close event
+    float confidence;        // running confidence 0..1 (F08 §6 D5)
+};
+
 ```cpp
```

Rendered end state (for reviewers):

```cpp
enum class EpisodePhase : uint8_t { Idle, Candidate, Active };
enum class EpisodeClass : uint8_t { Uncertain, Rest, Nap, MainSleep };
struct EpisodeStatus {
    TimeMs t_ms;                // when this status was produced
    TimeMs t_start_ms;          // monotonic start of the current/last episode; 0 when Idle
    EpisodePhase phase;
    EpisodeClass episode_class; // provisional while phase != Idle; final value is in the close event
    float confidence;           // running confidence 0..1
};
```

Placement rationale: `bw/state.h` is 02 §4.5's header (F00's one-header-per-section map, F00 §6 D4);
a new header would break that map for one struct.

## Alternatives considered (simplest vs scalable) and why rejected

- **Simplest — drop the live status, let the UI read closed episodes from the event log
  (F16/F17).** Rejected: F08 §3 mandates a live status, and a closed-only view cannot show an episode
  that is in progress (`Idle/Candidate/Active` is precisely the point).
- **Simplest for F08 — declare `EpisodeStatus` inside `bw_sleep` and hand the mailbox to F12.**
  Rejected: F12 may not include `bw_sleep` (01 §1 rule 1); it would also make the UI depend on one
  feature's header, i.e. the coupling the whole layering exists to prevent.
- **Most scalable — a provider interface `ISleepEpisodeProvider` in `bw_core` alongside the struct.**
  Deferred, not rejected on merit: it is the right step once there is a second producer, a second
  mailbox shape, or a consumer that must be faked behind DI. `AlertStatus` shows a struct + app-owned
  mailbox is sufficient for one producer/many readers today, and a `LatestValue<T>` instance is just as
  fake-able in a host test as a provider would be.
- **Chosen — additive struct only, app-owned `LatestValue<EpisodeStatus>`.** No interface growth, no
  breaking change, matches the existing `AlertStatus` pattern, and `EpisodePhase`/`EpisodeClass` are
  append-only enums per 02 §6.6, so later values (`Paused`, `Interrupted`, …) cost nothing.

## Affected features

  switched to `bw::EpisodeStatus` on acceptance.
- **F11 / F12** (consumers of the live status) — blocked until acceptance if they need the mailbox.
- **F00 / integrator** — applies the diff to `docs/interfaces/data_decision.md` §4.5 (and `docs/02`
  when synced); no `contract_checks.h` / `test_contract_layout.cpp` impact (purely additive, no
  existing offset, enum value or size changes).

## Migration / compatibility notes

Additive and append-only: no existing type, field, enum value, offset or serialized record changes,
so no stored event (`DEC-007` log) and no BLE characteristic (`02` §6.2/§6.3) is affected. `Event`
stays 48 bytes. The new enums start at `Uncertain == 0` / `Idle == 0` so zero-initialised memory
decodes as the honest default. Consumers that ignore the mailbox keep compiling unchanged.
