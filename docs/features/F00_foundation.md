# F00 — Foundation: repo, contracts-as-code, core library, test kit

> Track **E** · Milestone **M0** · Runs on **firmware + tools** · Read `docs/00_PROJECT_CONTEXT.md`, `00`, `01`, `02` first. Contract changes only via change request.

## 1. Responsibility
Create the skeleton every other feature builds on: PlatformIO project with `esp32dev` and `native` envs, the `bw_core` library implementing every type/interface in `02_INTERFACES.md`, static containers, `Result`, a `bw_testkit` library of fakes, an empty composition root with the wiring convention, CI scripts, and Python tooling skeletons. **Blocks all other features; keep it small and boring.**

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `docs/02_INTERFACES.md` (the specification of what to implement)

**Outputs (produced contracts)**
- `bw_core`: all types, enums, interfaces from `02` §1–5, `Result`, `RingBuffer`, `SpscQueue`, `LatestValue`, `IClock`
- `bw_testkit`: `FakeClock`, `FakeImu`, `FakePpg` (scripted/synthetic + CSV replay), `ScriptedButtons`, `RecordingLed`, `RecordingBuzzer`, `FakeCanvas`, `InMemoryKeyValueStore`, `InMemoryEventStore`, `RecordingFaultReporter`
- `AppContext` struct + `wire(AppContext&)` convention
- CI script: `pio test -e native`, `pio run -e esp32dev`, `ruff`, `clang-format --dry-run`
- Contract tests (sizeof/alignment/enum stability)
- CRs raised against `02` where it is ambiguous or unimplementable (§15) — the contract text itself is never edited here

## 4. Dependencies
- None (first feature).

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Contain any feature logic (no filtering, no state machines).
- Depend on Arduino/FreeRTOS inside `bw_core` value types (only the queue/mailbox wrappers may use a thin `Platform` shim, with a native fallback).
- Invent contract semantics; if `02` is ambiguous, raise a CR.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. How to implement containers/mailboxes so they work on host and device**
- Simplest: Header-only templates using `std::atomic` and a critical-section macro with a native no-op fallback.
- Professional/scalable: Abstract `IPlatformSync` with FreeRTOS and native implementations injected everywhere.
- Trade-offs: Second is cleaner but adds an interface to every container and noise for contributors.
- **Choice:** Header-only templates with one small `platform_sync.h` (`#ifdef ARDUINO`) — the only preprocessor switch in `bw_core`; documented in the header.

**D2. Where fakes live**
- Simplest: Each feature writes its own private fakes.
- Professional/scalable: One shared `bw_testkit` owned by F00.
- Trade-offs: Shared kit needs coordination, but private fakes drift apart and hide contract mismatches.
- **Choice:** Shared `bw_testkit`; features may add *private* helpers under their own `test/` but never edit the kit (CR/ask F00 owner).

---

**D3. How `02_INTERFACES.md` and `bw_core` are kept from drifting apart**
- Problem: `02` §4 says that after F00 the *code and its contract tests* are the contract, and `01` §6.3 requires a size/layout/serialization test per contract type. Without a mechanism, a field silently changes and no one notices until F17/F18 disagree about the binary layout.
- Simplest: implement `02` by hand and rely on review; contract tests are optional.
- Professional/scalable: two layers — (a) `static_assert` on `sizeof`, `alignof` and `std::is_trivially_copyable_v` for every contract type, in one `bw_core` header pulled in by an umbrella header so *any* consumer's build fails on drift; (b) Unity tests in `test/bw_core/` that pin every enum's numeric values with readable failure messages and exercise behaviour.
- Trade-offs: asserts in the library turn a legitimate contract change into a build error — desirable friction here, because `02` §6.6 requires a CR for exactly that change. Unity-only tests are easier to read but only help someone who runs the tests; asserts-only cannot express "these two enums must stay ordered".
- **Choice:** both layers. `bw/contract_checks.h` (compile-time, zero flash cost) + one Unity test file per contract area (§9). The `Event` assert is written against the **48-byte** layout CR-002 agreed on (§7/D7).

  | `02` section | Header |
  |---|---|
  | §1 conventions (`TimeMs`, `UnixSeconds`, `kSchemaVersion` policy) | `bw/types.h` |
  | §2 `Err`, `Result<T>`, `Result<void>` | `bw/result.h` |
  | §2 `RingBuffer`, `SpscQueue`, `LatestValue` | `bw/containers/*.h` |
  | §2 platform critical-section shim (D1) | `bw/platform_sync.h` |
  | §2 `IClock` | `bw/clock.h` |
  | time arithmetic helpers (D6) | `bw/time.h` |
  | §3 hardware-facing samples + `I*` device interfaces + `FontId` | `bw/hardware.h` |
  | §4.1 `DataQuality`, `SourceQuality` | `bw/quality.h` |
  | §4.2 `MotionFeatures`, `PpgFeatures` | `bw/features.h` |
  | §4.3 `ActivityContext`, `WearState` + estimates | `bw/context.h` |
  | §4.4 `FeatureVector`, `ModelOutput`, `IModel`, `Contribution` | `bw/model.h` |
  | §4.5 alertness/mode enums + `StateSnapshot` | `bw/state.h` |
  | §4.5 `EventType`, `UserLabel`, `Event` | `bw/events.h` |
  | §4.6 `AlertLevel`, `AlertStatus` | `bw/alerts.h` |
  | §4.6 `ButtonId` events, `ButtonGesture` | `bw/input.h` |
  | §4.6 `CommandType`, `Command` | `bw/commands.h` |
  | §5 providers (`IStateProvider` … `IBaselineProvider`, `BaselineProfile`, `HealthSnapshot`) | `bw/providers.h` |
  | §5 `IFaultReporter` + `FaultCode`/`FaultSeverity` (D8) | `bw/fault.h` |
  | §7 F00-defined `TaskHealth` | `bw/task_health.h` |
  | umbrella | `bw/core.h` |
  | compile-time contract asserts (D3) | `bw/contract_checks.h` |

**D5. `Result<T>` shape**
- Problem: `02` §2 gives `template <class T> struct Result { Err err; T value; bool ok() const; }` — a struct that always stores a `T`. That is a constraint (`T` must be default-constructible) *and* a footgun (on error, `value` is a default object a caller might read).
- Simplest: use `02`'s shape verbatim and document that `value` is only meaningful when `ok()`.
- Professional/scalable: tagged-union/`std::variant` storage so `T` need not be default-constructible, plus `value_or`, move semantics and `operator*`.
- Trade-offs: the variant removes the dummy value but drags in library machinery that fights `02` §1's "all structs are trivially copyable (safe for queues/mailboxes)", and without exceptions the error path still has to be hand-checked — so it buys little and costs the queue rule.
- **Choice:** `02`'s shape exactly, plus `[[nodiscard]]`, `constexpr bool ok() const`, and `static_assert(std::is_trivially_copyable_v<Result<T>> && std::is_default_constructible_v<T>)`. `Result<void>` is a specialization holding only `Err err` + `ok()`. Documented in the header: *"`value` is undefined unless `ok()`"* — this is the contract's own validity-flag rule (`02` §1) applied to the error path.

**D6. Time arithmetic helpers**
- Problem: `03` §4 requires host tests for `TimeMs` across large values and for huge time jumps; `01` §4 forbids 32-bit truncation. Raw `now - since` on `uint64_t` silently underflows to ~1.8e19 ms when a caller passes a stale or pre-reset timestamp, which makes *every* "has enough time passed" test pass.
- Simplest: raw `uint64_t` subtraction at each call site.
- Professional/scalable: `bw/time.h` helpers with saturating, documented semantics — `elapsedMs(now, since)` returns `0` when `now < since`; `withinMs(now, since, window)` returns `false` in the same case.
- Trade-offs: ~10 lines of `constexpr` helpers versus N call sites each re-deriving the comparison (and some getting it wrong). Helpers are trivially host-testable and cost nothing once inlined.
- **Choice:** helpers. All intermediate arithmetic stays `uint64_t`; no `uint32_t` is ever used for a duration. Note: `TimeMs` wraps after 2^64 ms ≈ 584,942,417 years, so true wrap is unreachable for this product — the *real* risks are 32-bit truncation and stale timestamps, both covered. Cross-boot comparisons are the caller's job: callers must compare only within one `boot_id` (`01` §4).

**D7. `Event` layout does not meet this spec's own 48-byte budget → CR-002**
- Problem: F00 §11 budgets `Event ≤ 48 bytes`. `02` §4.5's field order, with `TimeMs = uint64_t` forcing 8-byte alignment, produces **`sizeof(Event) == 56`**. Measured, not guessed — see the arithmetic below and the status log.
  ```
  as written in 02 §4.5:
    seq@0 boot_id@4 type@8 label@9 schema_version@10 [pad 5] t_start_ms@16
    duration_ms@24 t_start_unix@28 confidence@32 reasons@36 quality@40
    context@42 alertness@43 detector_id@44 payload@45 [pad 3]  =>  sizeof 56
  sum of member sizes = 48 exactly; padding = 5 + 3 = 8; 48 + 8 = 56
  ```
- Simplest: keep `02`'s order and raise this spec's §11 budget to 56 bytes (`[DERIVED]`), pinning `static_assert(sizeof(Event) == 56)`.
- Professional/scalable: reorder the same fields — `TimeMs` first, the three `uint8_t` enums and `detector_id`/`schema_version` last — giving `sizeof == 48`, `alignof == 8`, **zero padding**, no packing pragma, no type change, no field removed.
- Trade-offs: reordering changes the serialized layout of a record that `DEC-012` persists to flash and `02` §6.2/§6.3 transmits over BLE, so it is a **breaking** contract change and needs a CR — but no code, no stored record and no BLE traffic exists yet, so today it is free and after F00 is accepted it is expensive. Accepting 56 B costs 8 B (16.7 %) on *every* stored and synced event, forever, and puts `payload[8]` at the misaligned offset 45.
- **Choice:** raise **CR-002** proposing the reorder; keep the 48-byte budget and pin `static_assert(sizeof(Event) == 48)`. **CR-002 was accepted 2026-09-30**, so this is the agreed design; the integrator still has to write the diff into `02` §4.5 (§15). Rejected alternative kept on record: had the CR been refused, §11 would have become 56 bytes `[DERIVED]` and the assert 56. Verified with a standalone `g++ -std=c++17` probe printing `sizeof`/`alignof`/`offsetof` for both variants (recorded in the status log).

**D8. `FaultCode` and `FaultSeverity` initial sets**
- Problem: `02` §7 delegates both enums to F00 ("an initial set derived from F01/F23/F26 specs; afterwards append-only via CR"). Enums are append-only, so a wrong *split* cannot be corrected later without a breaking CR — the initial set is effectively permanent.
- Simplest: a handful of coarse categories (`Sensor, Bus, Quality, Storage, Comms, Power, System`) — easy to read, but F23 could not then tell an IMU fault from a PPG fault, so the first real feature forces a breaking re-split.
- Professional/scalable: one code per symptom *explicitly named in a current spec* (25 values, listed in §16 with its source spec), each value pinned by a contract test, plus `FaultSeverity` with one level per distinct response F26's policy needs.
- Trade-offs: coarse is smaller but cannot be refined without breaking; fine costs one byte per value in a lookup table and risks dead weight if a code is *invented* rather than derived. The guard against dead weight is the derivation rule, not a smaller list.
- **Choice:** fine-grained **but only for symptoms named today in F01/F23/F26/F11** — nothing speculated. Any later need arrives by append-only CR. `FaultSeverity = {Info, Warning, Error, Critical}`: four levels because F26 has four distinct responses (observe → degrade quality → fall back to a reduced pipeline → safe mode/watchdog). **F00 does not assign a severity per code** — that mapping is F26's degradation table (F26 §6 D2). Full lists in §16; they are listed in the status log as `02` §7 requires.

**D9. What `AppContext` carries at M0**
- Problem: `AppContext` is the composition root's aggregate (`01` §1 rule 3, `01` §2 wiring convention). F00 defines its skeleton; the integrator grows it. Guessing too much now bakes in wrong assumptions; leaving it empty pushes a one-line edit onto every future merge.
- Simplest: `struct AppContext {};` — empty, every feature's wiring adds its own members later.
- Professional/scalable: pre-declare every shared primitive now (clock, all queues and mailboxes, every provider pointer) so `app.h` never changes again.
- Trade-offs: empty means `app.h` grows one line per feature — an integrator-owned file, so conflicts are rare and the cost is real but small. Pre-declaring forces F00 to guess needs it cannot know (F00 has no feature to wire yet), and every wrong guess becomes an edit to a shared file under `03` §1's lock.
- **Choice:** minimal but not empty — carry only what M0 already mandates:
  - `bw::IClock* clock` — `01` §4 makes monotonic time available to every feature and `02` §2 defines `IClock`, so it is universally needed.
  - `const char* firmware_version` — F00's own DoD requires a version banner, and `02` §6.2 requires the BLE Device Info characteristic to carry the fw version; storing it here keeps one source of truth that F19 reads through wiring rather than by including an L5 header.
  Everything else is added by the integrator as features land. `AppContext` stays a plain aggregate with **no logic, no methods, no virtuals** (so it is not a contract type and needs no schema version). `kFirmwareVersion` lives in `app.cpp` — it is a version marker, not a threshold/rate/size, so the `00` §7.5 tag system does not apply to it.

**D10. How queue drop counters reach F23**
- Problem: F00 §10 requires that "container overflow … increments a drop counter readable by F23", and `01` §3.1 says overflow is "counted and reported". `02` §5 defines **no read path** from a queue to F23 — there is no queue-stats provider.
- Simplest: the counter lives on the container instance (`SpscQueue::drops()`, `LatestValue::overwrites()`); whoever holds the instance (the composition root, per D9) reads it, and the *producer* additionally calls `IFaultReporter::report()` the moment a drop happens, so the information reaches F26's registry with the dropping feature's context. **No contract change.**
- Professional/scalable: an `IQueueStatsProvider` + `QueueStats` snapshot in `02` §5 so F23/F26 see every queue centrally.
- Trade-offs: a central provider gives one place to look and matches the `HealthSnapshot` philosophy, but needs an additive CR, needs every producer to register its queues, and duplicates information already known at the drop site — where the useful context (which queue, what was lost, why) actually exists.
- **Choice:** simplest. The container accessor covers the local/host-test requirement; `IFaultReporter` covers the aggregation requirement; both exist already. **Separately observed, not fixed here:** `02` gives F26 a write-only `IFaultReporter` while `HealthSnapshot.fault_bits` is produced by F23 — so *no* read path exists from the fault registry back to F23. That gap belongs to F23/F26 to raise (`03` §6), not to F00.

**D12. Formatting baseline (`.clang-format`, `.editorconfig`)**
- Problem: CI will run `clang-format --dry-run`, so a style must exist and be pinned. The three skeleton files already in the repo use 4-space indent and attached braces, which is neither stock LLVM (2) nor stock Google (2).
- Simplest: adopt a stock style wholesale (LLVM or Google) and reformat the skeleton.
- Professional/scalable: derive the style from what the repository already contains and pin every key explicitly, so the result never depends on which `clang-format` version a contributor happens to have.
- Trade-offs: stock styles need a one-time reformat (churn is tiny — `main.cpp`, `app.h`, `app.cpp`), but silently change the project's look; a derived style means writing ~15 keys and no churn.
- **Choice:** derived and pinned — `BasedOnStyle: LLVM`, `IndentWidth: 4`, `ColumnLimit: 100`, attached braces, tab width 4, UTF-8, final newline, trim trailing whitespace. The exact key set is finalised **at Implement**, in the same change as the CI step, by running `clang-format --dry-run` against the existing skeleton and accepting only diffs we agree with — a style decided without running the tool is a guess. `.editorconfig` mirrors the same indent/charset/newline facts for editors; `00` §7.5's "no unexplained constants" is satisfied by a `# why:` comment on the one non-obvious key.

**D15. Where the TaskHealth update rule lives (recorded at Implement)**
- Problem: §9 requires `worst_lateness_ms` to "keep the max, never reset downward" — that is behavior, but `02` §7 (via §16) defines TaskHealth as a plain data struct with no operations. Without a home for the rule, every feature's task wrapper re-implements it.
- Simplest: leave `TaskHealth` a plain struct; each wiring file writes the fields itself.
- Professional/scalable: one `noteCycle(now, lateness, stack)` helper on the struct that stamps `valid`/`last_run_ms`, keeps the running max with `uint32_t`-in/`UINT16_MAX` saturation (a catastrophically late task must never truncate to a small number), and stores the stack mark as reported.
- Trade-offs: adding a member function makes §16's type slightly richer than `02`'s prose ("last run, max lateness, stack high-water mark" — it changes no fields), while the plain struct duplicates a three-line rule across F07/F10/F23 wrappers where one of them will eventually reset the max by mistake.
- **Choice:** the helper. Fields are untouched (§16 mirror updated in the same change); the rule is tested once in `test_task_health.cpp` instead of N times in feature tests. Default member initializers were added alongside so any construction starts `valid == false` (`02` §1), which costs nothing and removes an uninitialized-read path.

## 7. Module structure
Include root: `firmware/lib/bw_core/include/` → `#include <bw/…>` (mapping table in §6 D4).

```
firmware/lib/bw_core/
  include/bw/
    types.h result.h clock.h time.h platform_sync.h
    hardware.h quality.h features.h context.h model.h
    state.h events.h alerts.h input.h commands.h
    providers.h fault.h task_health.h
    containers/{ring_buffer,spsc_queue,latest_value}.h
    core.h            (umbrella)
    contract_checks.h (D3: static_assert layout/ABI/trivially-copyable)
  SPEC.md             (pointer to this spec)

firmware/lib/bw_testkit/
  include/bw/testkit/
    fake_clock.h fake_imu.h fake_ppg.h scripted_buttons.h
    recording_led.h recording_buzzer.h fake_canvas.h
    mem_kv_store.h mem_event_store.h recording_fault_reporter.h
    csv_row.h        (fixed-size parser, no heap)
  src/                (CSV row splitting implementation; host-only)

firmware/src/app/
  app.h     struct AppContext { bw::IClock* clock; const char* firmware_version; }   (D9)
  app.cpp   kFirmwareVersion; run() — integrator-owned after F00
  wiring/README.md     the wire_<feature>.{h,cpp} contract, one pair per feature

firmware/test/bw_core/    (one file per contract area, see §9)
```

`bw_testkit` is **host-only**. It is never included by `src/`, so PlatformIO's LDF will not build it for `esp32dev`; as a guard it `#error`s if compiled under `ARDUINO`, so an accidental include fails loudly instead of shipping a CSV parser to the device.

`kFirmwareVersion` (`app.cpp`) carries no tag: it is a version marker, not a threshold/timeout/rate/ML parameter/hardware behaviour, so `00` §7.5's tag system does not apply. All other values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan

| File | Cases |
|---|---|
| `test_result.cpp` | `ok()` on `Err::Ok` and on every failure code; `Result<void>`; `value` default-constructed on error (documents D5's "undefined unless `ok()`"); `Err` enum values pinned 0…11 in declaration order (append-only guard) |
| `test_ring_buffer.cpp` | empty first read; push to capacity; wrap-around pushes overwrite-oldest; indexed read ordering (index 0 = oldest) across a wrap; `N = 1` edge; `clear()`; ordering preserved with `T = Event`-sized payload; `static_assert` trivially copyable |
| `test_spsc_queue.cpp` | FIFO order across a wrap; `push` on full → refused **and** `drops()` increments, previously stored items untouched; `pop` on empty → `false`, no corruption; interleaved producer/consumer script reproducing `01` §3.1's "order matters, nothing lost silently" |
| `test_latest_value.cpp` | `get` before any `set` → `false` (no magic value, `02` §1); sequence number increments per `set`; intermediates dropped, newest wins; stored value copied verbatim |
| `test_time.cpp` | `elapsedMs` with operands straddling 2³² (no 32-bit truncation, `01` §4 / `03` §4); `now == since` → 0; `now < since` → 0 and `withinMs` → `false` (D6); huge forward jump; `TimeMs` at `UINT64_MAX - small` does not wrap in helpers |
| `test_contract_layout.cpp` | `static_assert` every contract struct's `sizeof`/`alignof`/trivially-copyable, including `sizeof(Event) == kEventBytes`; pin numeric values of `AlertnessState`, `WearState`, `ActivityContext`, `DataQuality`, `OperatingMode`, `PowerProfile`, `AlertLevel`, `ButtonGesture`, `EventType`, `UserLabel`, `ReasonCode`, `CommandType`, `SettingKey` (CR-003) |
| `test_fault_codes.cpp` | `FaultCode` and `FaultSeverity` values pinned exactly as §16 — this is the append-only guard `02` §7 asks reviewers to check |
| `test_task_health.cpp` | `valid == false` before the first cycle; `worst_lateness_ms` keeps the max, never resets downward; `stack_free_min_bytes` reported as measured |
| `test_providers.cpp` | `IStateProvider`/`IFeatureProvider` "no data yet" → `false`; `ISettings` cases (registry min/max/default clamping, unknown key → `Err::NotFound`, type mismatch → `Err::InvalidArg`) per **CR-003, accepted 2026-09-30**; `list()` truncation (`max` < registered → fills `max`, returns TOTAL) per **CR-006, accepted 2026-10-01** |
| `test_csv_row.cpp` *(bw_testkit)* | `FakeImu` replays a committed fixture CSV deterministically (same timestamps, same sample count on every run); malformed row → `Err::Corrupt`; over-long row → `Err::Overflow` (`kMaxCsvLineBytes`); missing columns → `Err::InvalidArg`, never a crash |

## 10. Failure behavior
- Container overflow never corrupts data; it increments a drop counter readable by F23 (read path in D10).
- Never crash on missing/invalid input; return `Err` or an output flagged `Insufficient`.

## 11. Budget
Header-only, ~0 KB flash beyond used templates; **static RAM contribution 0 B** beyond instantiated containers (no globals, no buffers of F00's own). `Event` size target **≤ 48 bytes** `[DERIVED]` (bounds flash log growth) — the arithmetic is in D7 (sum of member sizes = 48 B exactly; the printed order wastes 8 B of padding), and **CR-002 was accepted 2026-09-30**, so the reorder is agreed and 48 B holds. CPU: negligible.

*Measurement status: **measured 2026-10-01** (Implement + Test): `xtensa-esp32-elf-size` on the linked `esp32dev` image gives text 197217 · data 70192 · bss 4937 · dec 272346; stripping `#include <bw/core.h>` from `app.cpp` changes nothing (**Δ 0 B flash, 0 B RAM**) — header-only with no container instantiated on the device yet, exactly as predicted. Full-image totals: RAM 21464 B (6.6 %), Flash 267153 B (20.4 %). `Event` size is pinned by `static_assert(sizeof(Event) == kEventBytes)` in `bw/contract_checks.h` and `test_contract_layout.cpp` (48 B, CR-002).* The Δ becomes non-zero the first time a feature instantiates a container; re-run the same strip-and-compare then.

## 12. Out of scope
- Any sensor/driver code (F01).
- Task creation (per-feature wiring).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- `esp32dev` firmware builds with an empty loop and prints a version banner.
- `docs/02_INTERFACES.md` §7 types defined and listed in the status log.
- Fresh clone → `pio test -e native` green with one command.

## 15. Change requests raised by F00 (contracts never edited directly)
| CR | Title | Type | Blocks | Status |
|---|---|---|---|---|
| CR-002 | Reorder `Event` fields so `02` §4.5 meets F00 §11's 48-byte budget (56 B → 48 B) | breaking (zero migration: nothing implemented, stored or sent) | Freezing `bw_core`'s `events.h` layout and the `kEventBytes` contract test | **accepted 2026-09-30** — diff still to be applied to `02` |
| CR-003 | Give `ISettings` and `Command::SetSetting` an implementable signature | clarification | Implementing `bw/providers.h`'s `ISettings`; F18/F24/F25/F12 settings screens | **accepted 2026-09-30** — diff still to be applied to `02` |
| CR-004 | Split `Err::Overflow` from `Err::Full` with a usage table | clarification | Nothing in M0; prevents divergent usage across F17/F18/F19 | **accepted 2026-09-30** — diff still to be applied to `02` |

All three accepted by the developer/integrator on 2026-09-30. **Applying an accepted diff to `02_INTERFACES.md` is the integrator's step, not F00's** — `02`'s header says *"Do not edit in a feature session"*, and CR-001's precedent is that the developer applies the accepted diff. Until then F00 implements against these CRs and never edits the contract (`03` §5).

## 16. `02_INTERFACES.md` §7 — types F00 was asked to define
`02` §7 delegates these to F00 and requires them to be **listed in the status log** so reviewers can check them against F01/F23/F26. Proposed here at Define; the code lands at Implement; every value is pinned by `test_fault_codes.cpp` / `test_contract_layout.cpp` (§9) so the append-only rule is enforced mechanically.

**`FontId`** (used by `IDisplayCanvas::drawText`, `02` §3) — exactly the three values `02` §7 names, no more:
```cpp
enum class FontId : uint8_t { Small, Medium, Large };
```

**`Contribution`** (used by `IModel::contributions`, `02` §4.4) — field-for-field as `02` §7 specifies:
```cpp
struct Contribution { uint8_t feature_index; float weighted_value; };
```

**`TaskHealth`** — `01` §3.2 mandates exactly three contents: *"last run, max lateness, stack high-water mark"*. The fourth field is not an invention but `02` §1's rule that *"'Not available' is expressed by a validity flag … never by a magic number"*: without it, `last_run_ms == 0` would be indistinguishable between "ran at boot" and "never ran". `worst_lateness_ms` is `uint16_t` to match `HealthSnapshot::worst_task_lateness_ms` (`02` §5), so no conversion is needed at the boundary. Default initializers make *any* construction start in the "no data" state, and `noteCycle` holds the §9-pinned max-keeping rule so no task wiring can re-invent it wrong (lateness `uint32_t` in, saturating at `UINT16_MAX` — a late task records as very late, never as a truncated small number):
```cpp
struct TaskHealth {
  bool     valid = false;            // false until the task completes one cycle (02 §1 validity rule)
  TimeMs   last_run_ms = 0;          // monotonic time of the last completed cycle
  uint16_t worst_lateness_ms = 0;    // worst (actual - expected) period since boot; feeds HealthSnapshot
  uint16_t stack_free_min_bytes = 0; // minimum free stack seen (FreeRTOS high-water mark); 0 = not yet measured

  // One completed cycle: stamps last_run, keeps the worst lateness as a running max
  // (never resets downward), saturates at UINT16_MAX instead of wrapping.
  void noteCycle(TimeMs now_ms, uint32_t lateness_ms, uint16_t stack_free_bytes);
};
```

**`FaultSeverity`** — `02` never defines it; F00 does. Four levels, one per *distinct response* F26's policy can take, so no level is speculative:
```cpp
enum class FaultSeverity : uint8_t {
  Info,      // record for diagnostics; no capability change            (e.g. BatterySenseUnavailable)
  Warning,   // still functioning, F23 downgrades quality              (e.g. PpgPoorContact, LowBattery)
  Error,     // source unusable; F26 degrades to a reduced pipeline     (e.g. ImuReadFailed, DisplayFailed)
  Critical   // system risk: safe mode / watchdog path implicated       (e.g. BootLoop, WatchdogReset, Brownout, CriticalBattery)
};
```
**F00 deliberately does *not* assign a severity to each code.** That mapping is policy, and policy belongs to F26's degradation table (F26 §6 D2); F00 only supplies the vocabulary.

**`FaultCode`** — derived strictly from symptoms a current spec names (`02` §7). 25 values, source-tagged; **nothing speculated**, so no value is dead weight. Append-only afterwards, via CR.
```cpp
enum class FaultCode : uint8_t {
  None = 0,                 // "no fault" — avoids callers inventing a 0-means-what convention
  // ── bus / HAL ────────────────────────────────────────────────────────────────────────
  I2cTimeout,               // F01 D1: per-transaction timeout
  I2cBusStuck,              // F01 D1: stuck-bus recovery (9 SCL pulses) could not clear it
  ImuAbsent,                // F01 §10 absent at boot; A-05 WHO_AM_I != kExpectedMpuWhoAmI
  ImuReadFailed,            // F01 §10 kFailuresBeforeFailed consecutive read failures -> SensorStatus::Failed
  PpgAbsent,                // F01 §10 absent at boot; part ID != kExpectedMax30102PartId
  PpgReadFailed,            // F01 §10 consecutive read failures -> SensorStatus::Failed
  PpgFifoOverrun,           // F01 §9 FIFO overflow flag: FIFO not drained within kPpgFifoPollMaxIntervalMs
  DisplayFailed,            // F01 §10 display failure must never block sensing
  BatterySenseUnavailable,  // F01/A-04 no ADC divider -> battery reported "unknown", not a guess
  ButtonStuck,              // F11: pressed > 10 s -> ignore the input and report
  // ── data quality ─────────────────────────────────────────────────────────────────────
  ImuStuckSensor,           // F23 D2: run-length of identical values > kStuckSamples
  PpgFlatline,              // F23 D2: flat-line check
  PpgSaturated,             // F23 D2: PpgSample::saturated held across a window
  PpgPoorContact,           // F23 §3: poor contact / low perfusion
  TaskOverrun,              // F23 §8: lateness > kMaxTaskLatenessMs -> processing overload
  LowHeap,                  // F23 §3: heap statistics below headroom
  LowBattery,               // F23 §8: battery < kLowBatteryPercent
  CriticalBattery,          // F23 §8: battery < kCriticalBatteryPercent
  // ── reliability ──────────────────────────────────────────────────────────────────────
  TaskStalled,              // F26 D1: heartbeat missing inside its deadline window
  WatchdogReset,            // F26 §3: reset reason capture shows a watchdog reset
  Brownout,                 // F26 §6: brownout protection tripped
  BootLoop,                 // F26 §6: >= kMaxRebootsInWindow inside kRebootWindowMin -> safe mode
  StorageDegraded,          // F26 §3: degraded flag "storage degraded"
  BleDown,                  // F26 §3: degraded flag "BLE down"
};
```
**Deliberately excluded, and why:** F26's *"IMU lost / PPG lost"* degraded flags are **derived**, not new codes — they are the consequence of `ImuReadFailed`/`PpgReadFailed`, and adding them would duplicate one condition under two names. Nothing from F17/F18/F19/F20 appears, because no current spec names a storage or BLE *fault* symptom; those arrive by append-only CR when their features specify them.
