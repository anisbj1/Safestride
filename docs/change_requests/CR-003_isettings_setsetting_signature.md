# CR-003 — Give `ISettings` and `Command::SetSetting` an implementable signature

- Raised by: F00 / 2026-09-30
- Type: **clarification**
- Problem: F00's responsibility (`F00_foundation.md` §1) is to implement **every type and interface in `02_INTERFACES.md` §1–5** as code, and its §5 "Must not" forbids *inventing contract semantics*. `02` §5 does not give `ISettings` a signature at all — only a prose comment:

  ```cpp
  class ISettings       { /* typed get/set over registered keys with min/max/default; change notifications via Command::SetSetting */ };
  ```

  Two things are therefore unimplementable as written:

  1. **How a key is identified.** "Registered keys" is not a type. `IKeyValueStore` (the persistence layer directly below) takes `const char* key`, so strings are the obvious candidate — but a string cannot travel in `Command::SetSetting`, whose entire payload is two `uint32_t`s:
     ```cpp
     struct Command { CommandType type; TimeMs t_ms; uint8_t source; /*Button|Ble|Internal*/ uint32_t arg0; uint32_t arg1; };
     ```
     There is no room for a key *and* a value, so `SetSetting` cannot carry a string key, and no rule says what `arg0`/`arg1` mean for it.
  2. **How min/max/default are read.** A settings screen (F12), the BLE `Config` characteristic (F19) and a defaults/reset path (F18/F24) all need the range table, but no interface exposes it.

  Left unresolved, F00 would have to invent all three, and F18/F19/F24/F25/F12 would each invent a different answer — exactly the divergence the contract layer exists to prevent.

- Proposal: exact diff to `docs/02_INTERFACES.md`.

  **(a) new value types, inserted directly above `class ISettings` in §5:**

  ```cpp
  enum class SettingKey : uint16_t { /* registered settings; append only, never renumber */ };
  enum class SettingType : uint8_t { Bool, Int, Float };
  struct SettingValue {                             // trivially copyable; only the member matching `type` is meaningful
    SettingType type;
    union { bool b; int32_t i; float f; };
  };
  struct SettingSpec {                              // the registration table: one entry per key
    SettingKey  key;
    SettingType type;
    SettingValue min;                               // bounds expressed in the value's own type
    SettingValue max;
    SettingValue default_v;
    const char* name;                               // stable identifier for IKeyValueStore and the BLE Config characteristic
  };
  ```

  **(b) replace the `ISettings` comment with:**

  ```cpp
  class ISettings {                                 // registration table is fixed at wiring time; lookups are O(1)
   public:
    virtual Result<SettingValue> get(SettingKey) const = 0;             // Err::NotFound for an unregistered key
    virtual Result<void>         set(SettingKey, const SettingValue&) = 0;  // type mismatch -> Err::InvalidArg;
                                                                           // outside min..max -> clamped, not rejected
    virtual size_t               list(SettingSpec* out, size_t max) const = 0;  // fills the range/default table (UI + reset)
  };
  ```

  **(c) semantics of the existing `Command::SetSetting`, documented at `enum class CommandType` in §4.6 (no field changes):**

  ```
  SetSetting: arg0 = static_cast<uint32_t>(SettingKey); arg1 = the value's raw 32 bits —
  Bool: 0 / 1 · Int: static_cast<uint32_t>(int32 bits) · Float: IEEE-754 bit pattern.
  A receiver resolves the key's SettingType from ISettings::list() and decodes arg1 accordingly;
  an unrecognised key is dropped (Err::NotFound), never guessed at.
  ```

- Alternatives considered (simplest vs scalable) and why rejected:
  - **Simplest: `ISettings` takes `const char*` keys, matching `IKeyValueStore` exactly.** Rejected: it makes the contract internally inconsistent with `Command::SetSetting` (§4.6), which has no string storage — so `SetSetting` would still be undefined and the whole point of the CR would remain unsolved.
  - **Simplest alternative for the command: add a settings payload to `Command`.** Rejected: `Command` is a compact, fixed-size record transmitted in the BLE `Command` characteristic (`02` §6.2); growing it changes its serialized layout (breaking) to solve something that fits in the two words it already has.
  - **Scalable: a generic variant/`std::any`-style settings value with string keys and dynamic registration at runtime.** Rejected: `02` §1 requires contract structs to be trivially copyable with no `String` and no `std::vector`, and runtime registration would mean heap use in a path F24/F25 run from. It also buys nothing — this device will have tens of settings, not thousands.
  - **Chosen: an append-only `SettingKey` enum plus a fixed `SettingSpec` range table.** It fits the existing 8-byte `Command` payload untouched, keeps every contract struct trivially copyable, uses the project's own append-only-enum rule (`02` §6.6), and gives F12/F19 a range table to render rather than each re-deriving limits. The `const char* name` in `SettingSpec` is a pointer to a static literal, which `IKeyValueStore` already relies on, so it does not violate the no-owned-memory rule.

- Affected features: F00 (must implement `bw/providers.h`) · F18 (`KvStore`/settings persistence) · F19 (BLE `Config` R/W characteristic) · F24 (mode/profile settings) · F25 (power thresholds) · F12/F11 (settings screens and `SetSetting` from buttons) · F20 (settings travel over sync).

- Migration / compatibility notes: **None.** No implementation of `ISettings` exists; `Command` and `CommandType` are unchanged in layout and in enumeration (no value added, removed or reordered). The three additions (`SettingKey`, `SettingType`, `SettingValue`, `SettingSpec`) are new names in `02` §5 — additive by `02` §6.6. First concrete keys (`SampleRateHz`, `DisplayTimeoutMs`, …) are **not** enumerated here: each is registered by the feature that owns it, appended at the time it lands, so this CR does not pre-commit anyone's settings list.

- Status: **accepted** — 2026-09-30, by the developer/integrator.
- Application: **applied by the integrator 2026-10-01** to `docs/interfaces/services.md` §5 (value types + `ISettings`) and `docs/interfaces/data_decision.md` §4.6 (`SetSetting` semantics) — split of `02` by CR-005.
