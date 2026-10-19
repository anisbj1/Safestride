# CR-002 — Reorder `Event` fields so §4.5 meets F00's 48-byte budget

- Raised by: F00 / 2026-09-30
- Type: **breaking** (no migration required — see below)
- Problem: `docs/features/F00_foundation.md` §11 sets the budget **`Event` size target ≤ 48 bytes `[EXPERIMENTAL]`** ("bounds flash log growth"). The field order printed in `02_INTERFACES.md` §4.5 cannot meet it.

  With `TimeMs = uint64_t` (`02` §1) requiring 8-byte alignment, the declaration as written lays out as:

  | field | offset | size |
  |---|---|---|
  | `seq` | 0 | 4 |
  | `boot_id` | 4 | 4 |
  | `type` | 8 | 1 |
  | `label` | 9 | 1 |
  | `schema_version` | 10 | 1 |
  | *(padding)* | 11 | **5** |
  | `t_start_ms` | 16 | 8 |
  | `duration_ms` | 24 | 4 |
  | `t_start_unix` | 28 | 4 |
  | `confidence` | 32 | 4 |
  | `reasons` | 36 | 4 |
  | `quality` | 40 | 2 |
  | `context` | 42 | 1 |
  | `alertness_at_start` | 43 | 1 |
  | `detector_id` | 44 | 1 |
  | `payload[8]` | **45** | 8 |
  | *(padding)* | 53 | **3** |
  | **`sizeof`** | | **56** |

  Sum of member sizes = **48 B exactly**; padding = 5 + 3 = **8 B**; `sizeof(Event)` = **56 B** — so the whole 8-byte gap is padding, and `payload` lands at the misaligned offset 45.

  **Measured, not reasoned:** a standalone `g++ -std=c++17` probe printing `sizeof`/`alignof`/`offsetof` for the struct exactly as printed above returns `sizeof=56 alignof=8`, and `sizeof=48 alignof=8` for the reordered variant below. (Probe file kept outside the repository, in the session temp dir.)

- Proposal: exact diff to `docs/02_INTERFACES.md` §4.5 — **same fields, same types, same names, same meanings; only the declaration order changes.**

  ```diff
   struct Event {                                   // fixed-size record, persisted + transmitted; version it
     static constexpr uint8_t kSchemaVersion = 1;
  -  uint32_t seq;            // assigned by F17, monotonic per device, never reused
  -  uint32_t boot_id;        // increments each boot
  -  EventType type; UserLabel label; uint8_t schema_version;
  -  TimeMs t_start_ms; uint32_t duration_ms;       // monotonic within boot_id
  -  UnixSeconds t_start_unix;                       // 0 if unknown at creation time
  -  float confidence;                               // 0..1, model/heuristic confidence, NOT clinical certainty
  -  ReasonMask reasons; SourceQuality quality; ActivityContext context;
  -  AlertnessState alertness_at_start; uint8_t detector_id; uint8_t payload[8];  // type-specific small payload
  +  // Field order is load-bearing: TimeMs first (8-byte aligned), all 1-byte fields last,
  +  // so the struct packs to exactly kEventBytes (48) with zero padding and no packing pragma.
  +  TimeMs t_start_ms;             // offset 0  monotonic within boot_id
  +  uint32_t seq;                  // offset 8  assigned by F17, monotonic per device, never reused
  +  uint32_t boot_id;              // offset 12 increments each boot
  +  uint32_t duration_ms;          // offset 16
  +  UnixSeconds t_start_unix;      // offset 20 0 if unknown at creation time
  +  float confidence;              // offset 24 0..1, model/heuristic confidence, NOT clinical certainty
  +  ReasonMask reasons;            // offset 28
  +  uint8_t payload[8];            // offset 32 type-specific small payload (8-byte aligned)
  +  SourceQuality quality;         // offset 40
  +  EventType type;                // offset 42
  +  UserLabel label;               // offset 43
  +  ActivityContext context;       // offset 44
  +  AlertnessState alertness_at_start;  // offset 45
  +  uint8_t detector_id;           // offset 46
  +  uint8_t schema_version;        // offset 47
   };
  ```

  Resulting layout: `sizeof(Event) == 48`, `alignof == 8`, **0 bytes of padding**, `payload` 8-byte aligned. F00 pins it with `static_assert(sizeof(Event) == kEventBytes)`.

- Alternatives considered (simplest vs scalable) and why rejected:
  - **Simplest: keep §4.5 exactly as written and raise F00 §11's budget to 56 bytes `[DERIVED]`.** Rejected because it is a permanent 16.7 % cost on every event this device ever stores to flash (DEC-007's append-only segmented log) and ever transmits over BLE (`02` §6.3's `Sync Data` chunks), for zero benefit — and it would leave `payload[8]` at the unaligned offset 45, where any 4-byte access into the payload via a cast is a misaligned read on Xtensa.
  - **Scalable: change `TimeMs` to `uint32_t` so everything fits in 32-bit alignment.** Rejected: `TimeMs` is a foundational primitive (`02` §1) already relied on by `01` §4's monotonic-ms model and by F00's own DoD test for values beyond 32 bits; `uint32_t` wraps after ~49.7 days of uptime, which `01` §4 does not permit.
  - **Rejected: `#pragma pack` / `__attribute__((packed))` to reach 48 B with the current order.** Rejected: unaligned 8-byte accesses cost cycles on Xtensa, packed structs are fragile across compilers, and they defeat `02` §1's "trivially copyable, safe for queues/mailboxes" intent.
  - **Chosen: reorder the fields.** No field is added, removed, renamed or retyped; no packing pragma; the budget in F00 §11 holds as written; and it costs nothing today because nothing is implemented, stored or transmitted yet.

- Affected features: F00 (contract test target), F17 (assigns `seq`), F18 (persists the record), F19/F20 (BLE + sync transfer), F21 (decoder), F08/F09 (producers), and F28 (Python event decoder). **All of them are at `(not started)`** with no code, no stored records and no BLE traffic, so the only action required of them is to be notified.

- Migration / compatibility notes: **None required.** `Event::kSchemaVersion` stays **1** — version 1 was never released, never written to flash and never sent over the air, so there is no record in the old layout to migrate. The Python decoder (F28) and the BLE `Sync Data` chunk format (F20) are written against whatever layout exists at implementation time, and both do not exist yet. The change is classified `breaking` because `DEVELOPMENT.md` §6.6 defines reordering fields as breaking; its practical cost right now is zero, which is precisely why it should be settled **before** F00 freezes `bw_core`'s public headers (`03` §1).

- Status: **accepted** — 2026-09-30, by the developer/integrator.
- Application: **applied by the integrator 2026-10-01** to `docs/interfaces/data_decision.md` §4.5 (split of `02` by CR-005); the `kEventBytes` contract test in F00 asserts 48.
