# CR-021 — Pin the `SetTime` command argument layout (`arg0 = unix_seconds`)

- Raised by: F21 / 2026-10-17 (wave 5 plan session, branch `wave/5/F21`)
- Type: clarification
- Problem: `docs/interfaces/external.md` §6.1 says "Phone sends `SetTime(unix_seconds)`", and `BLE_GATT.md` §4.3 defines the 9-byte Command image as `[type u8][arg0 u32 LE][arg1 u32 LE]` — but **no contract line pins which argument carries the seconds**. Only `SetSetting` (CR-003: `arg0` = key, `arg1` = value) and `ConfirmEvent`/`RejectEvent` (`arg0` = Event::seq, F14 §13) have pinned args; `CommandType::SetTime` = 11 exists (`data_decision.md` §4.6) with no arg rule. Evidence: grep for `SetTime`/`arg0` across `docs/` and `firmware/` finds no consumer — no firmware code decodes a SetTime command yet. F21 (wave 5) is the **first sender**, and the firmware consumer will be written later; without a pin the two ends can silently diverge (device reads `arg1` = 0, or ignores the time), which would only show up as "clock never syncs" on the bench.
- Proposal: exact addition to `docs/interfaces/data_decision.md` §4.6, next to the existing CR-003 comment block:

  ```cpp
  // SetTime (CR-021): arg0 = unix_seconds (UnixSeconds, u32 LE on the wire);
  //   arg1 = 0, reserved. Set by the companion once per connect; the device
  //   stamps TimeSync{boot_id, t_mono_ms, unix_s} (external.md 6.1).
  ```

- Alternatives considered (simplest vs scalable) and why rejected:
  - Simplest: `[ASSUMED] arg0 = unix_seconds` in F21's spec only — works until the firmware consumer is written, possibly by another contributor with no reason to read F21's spec; rejected because a wire-format guess between two not-yet-existing implementations is exactly what the contract layer exists to prevent.
  - Scalable: a versioned command payload redesign — rejected as breaking and unnecessary for a single u32.
  - Choice: one clarification line in the locked doc, zero code change (no implementation exists on either side).
- Affected features: F21 (sender), the firmware SetTime/clock consumer (future), F19 (passthrough only — no change: F19 is a dumb byte pipe, CR-019).
- Migration / compatibility notes: none — no sender or consumer is implemented yet (grep evidence above). The eventual firmware consumer must read `arg0`.
- Status: **accepted 2026-10-17** (W5 G1, integrator; clarification, additive, zero code). Applied to `docs/interfaces/data_decision.md` §4.6 on `master` `27ad169`.
