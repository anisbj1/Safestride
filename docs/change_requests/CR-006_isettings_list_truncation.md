# CR-006 — Pin `ISettings::list()` truncation convention: returns the total registered count

- Raised by: F00 (open question recorded in `F00_foundation.md` §13) / 2026-10-01
- Type: **clarification** (no layout, no signature change)
- Problem: CR-003 gave `ISettings` the method

  ```cpp
  virtual size_t list(SettingSpec* out, size_t max) const = 0;  // fills the range/default table (UI + reset)
  ```

  but pinned neither CR-003 nor `02` says what the return value means when the caller's buffer
  (`max`) is smaller than the number of registered keys. Two readings are both "correct" under
  the current text:

  1. return the number of entries **written** (`min(max, registered)`), or
  2. return the **total registered count**, having written only the first `max`.

  This is not academic: a settings screen (F12) or the BLE `Config` characteristic (F19) that
  allocates room for 10 `SettingSpec`s while the device has 20 would, under reading 1, be told
  "10 — that's all of them" and would silently render half the menu with no indication that
  settings are missing. F00 must not invent contract semantics (`F00_foundation.md` §5), so it
  deliberately left `list()` untested pending this answer.

- Proposal: **reading 2** — document at `ISettings::list()` that it always returns the total
  number of registered keys and fills at most `max` entries:

  ```cpp
  class ISettings {                                 // registration table is fixed at wiring time; lookups are O(1)
   public:
    virtual Result<SettingValue> get(SettingKey) const = 0;             // Err::NotFound for an unregistered key
    virtual Result<void>         set(SettingKey, const SettingValue&) = 0;  // type mismatch -> Err::InvalidArg;
                                                                           // outside min..max -> clamped, not rejected
    // Returns the TOTAL number of registered keys (never less than what is written) and fills at
    // most `max` entries, first `max` in registration order. `returned > max` therefore means
    // "truncated — call again with a bigger buffer". Same convention as snprintf()/va_list.
    virtual size_t               list(SettingSpec* out, size_t max) const = 0;
  };
  ```

- Alternatives considered and why rejected:
  - **Return the number written.** Rejected (owner's option B): truncation becomes undetectable —
    a UI can silently omit settings, which is exactly the divergence the contract layer exists to
    prevent. Also forces a second, size-query call to be invented anyway.
  - **Return an error (`Err::NotFound` / `Err::InvalidArg`) when `max` is too small.** Rejected
    (owner's option C): the caller must already know the count to size the buffer correctly, so
    the error arrives one step too late and pushes a get-count-then-call dance onto embedded
    callers.
  - **Chosen: return the total registered count, write at most `max`** (owner's option A): the
    `size_t` return slot already exists, nothing about the signature changes, and it matches the
    familiar `snprintf()` convention.

- Affected features: F00 (contract test + `bw/providers.h` implementation may now pin this) · F12/F11 (settings screens) · F19 (BLE `Config`) · F18/F24 (defaults/reset path).

- Migration / compatibility notes: **None.** No implementation of `list()` exists yet; signature
  and layout unchanged; the addition is a comment clarifying an existing method.

- Status: **accepted** — 2026-10-01, decided by the owner (option A) via the integrator.

- Application: **applied by the integrator 2026-10-01** to `docs/interfaces/services.md` §5.
  The `list()` convention is now contract; F00's previously parked open question is answered and
  may be tested like any other rule.
