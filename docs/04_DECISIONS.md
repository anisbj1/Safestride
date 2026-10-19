# 04 — DECISIONS, ASSUMPTIONS, OPEN QUESTIONS

> Cross-feature decisions only. Feature-local ones live in the feature spec. Appended by the integrator.
> Format per decision: **Problem → Alternatives (simplest / scalable) → Trade-offs → Choice**.

---

## 1. Decision log

### DEC-001 — Firmware toolchain: PlatformIO + Arduino-ESP32 **[DECIDED, confirm]**
- Problem: build system + framework for multi-module firmware with host tests.
- Alternatives: (simplest) Arduino IDE — single-folder, weak library/test support, pushes toward giant `.ino`. (scalable) ESP-IDF native — full control, partition/OTA/power APIs, steeper curve, fewer ready drivers. (middle) PlatformIO + Arduino core — library structure, `native` test env, still allows ESP-IDF APIs.
- Trade-offs: Arduino-core adds abstraction overhead and hides some IDF config; IDF alone slows sensor bring-up.
- Choice: **PlatformIO + Arduino-ESP32** (uses IDF underneath; IDF calls allowed inside HAL/OTA/power). Project rule "avoid giant `.ino`" is satisfied structurally.

### DEC-002 — C++17, no exceptions/RTTI, static allocation after startup **[DECIDED]**
- Problem: determinism and memory safety on ~520 KB SRAM with BLE.
- Alternatives: (simplest) idiomatic Arduino `String`/`new` freely. (scalable) static allocation + fixed containers + `Result<T>`.
- Choice: static + `Result<T>`; readability preserved with small typed containers rather than clever templates.

### DEC-003 — Host-testable pure logic **[DECIDED]**
- Problem: verifying logic without hardware and in parallel.
- Alternatives: (simplest) test on device only. (scalable) `native` PlatformIO env with Unity + fakes + replay.
- Choice: native env; only HAL/BLE/storage-driver glue is device-only. Cost: discipline in keeping platform calls out of logic.

### DEC-004 — State model split into independent axes **[DECIDED]**
- Problem: the original list mixed alertness, activity, wear, quality, events and mode.
- Alternatives: (simplest) one big enum. (scalable) orthogonal axes composed by F07.
- Trade-offs: more types; avoids a combinatorial enum and invalid combos ("not worn but drowsy").
- Choice: axes as in `00` §6. Suspected events are `Event` records, not states.

### DEC-005 — Companion application stack **[DECIDED — proposal accepted; scope/stack interim ruling 2026-10-17]**
- Problem: phone/desktop client for sync, visualization, config, export.
- Alternatives: (simplest) Python desktop tool with `bleak` + local SQLite + notebooks/Streamlit — great for research, not a wearable companion. (scalable) Flutter mobile app, local-first SQLite, BLE via `flutter_reactive_ble`/`flutter_blue_plus`. Others: native Android (Kotlin) — best BLE control, Android only; Web Bluetooth PWA — no iOS Safari support; Django backend — requires server, duplicates embedded logic, and is not needed for local-first.
- Proposal: **Flutter, local-first, no server**, with the Python `bleak` tool from F28 as the day-one research path.
- **W5 interim ruling (developer, 2026-10-17):** **Python + `bleak` + local SQLite desktop app now** (day-one path became the actual path; host-testable, doubles as the M4 MTU≥55 bench central), **Flutter deferred** to a later wave; scope = **minimal per §3 Q1** ("nothing fancy": pairing, live status, synced timeline, sync control, time sync — no trends/settings/labels/diagnostics screens yet).
- Impact: F21 (all), F15/F16/F20 companion sides.

### DEC-006 — First ML model family **[DECIDED, revisit after data]**
- Problem: model that runs on ESP32, explainable, trainable with little data.
- Alternatives: (simplest) hand-tuned heuristic scores. (mid) logistic/softmax regression or small decision tree/forest exported as constant arrays. (scalable) TensorFlow Lite Micro / neural nets.
- Trade-offs: NN needs data and RAM we do not have; heuristics do not learn.
- Choice: **heuristic v0 (M2) → linear/tree model (M5)** behind `IModel`; TFLM only if evaluation shows a clear gain.

### DEC-007 — Local persistence: LittleFS + own event-log format **[DECIDED]**
- Alternatives: (simplest) NVS key-value only; (mid) LittleFS files with fixed-size CRC'd records, segmented; (scalable) SQLite on flash.
- Trade-offs: NVS wears and does not suit append logs; SQLite is heavy for this MCU.
- Choice: LittleFS, append-only segmented log, ring-overwrite of oldest *synced* segments first.

### DEC-008 — BLE stack: NimBLE-Arduino **[DECIDED]**
- Alternatives: default Bluedroid (larger RAM/flash), NimBLE (smaller, supports bonding/LE Secure Connections).
- Choice: NimBLE; classic BT disabled in build to save RAM.

### DEC-009 — Time model: monotonic + phone-provided wall clock **[DECIDED]**
- Problem: no RTC. Alternatives: external RTC (hardware change), NTP (needs Wi-Fi), phone time over BLE.
- Choice: phone time via `SetTime`; events keep `boot_id` + monotonic time so nothing is lost when time is unknown.

### DEC-010 — No Django / no server backend **[DECIDED, replaces original text]**
- The original document mentioned Django. This project is local-first and offline-capable; a server adds hosting, privacy exposure and duplicated logic. If a server is ever wanted (multi-user research), it is a **new** feature under a CR, and Django conventions would then apply.

### DEC-011 — Sync = cursor + ack, idempotent **[DECIDED]** (see `02` §6.3)

### DEC-012 — Event record = fixed-size, versioned, append-only **[DECIDED]**
- Simplest: text/JSON lines. Scalable: fixed binary + schema version + CRC. Choice: binary — bounded flash use, deterministic parsing, cheap BLE transfer; a Python decoder ships with F28.

### DEC-013 — Alert engine never reads ML output **[DECIDED]**
- Alerts derive from `StateSnapshot` + event notifications + commands. Keeps ML replaceable and alert behavior deterministic and testable.

### DEC-014 — Feature assembly lives with the estimator (F06), inference is model-agnostic (F05) **[DECIDED]**
- Prevents F05 from knowing about sensors and F06 from knowing model internals; the **feature schema file** is the handshake.

### DEC-015 — Display library: U8g2 (inside F01 only) **[DECIDED, confirm controller]**
- Alternatives: Adafruit_SSD1306 (simplest for SSD1306), U8g2 (supports SSD1306 *and* SH1106 clones — common on cheap 0.96" modules). Choice: U8g2, hidden behind `IDisplayCanvas`.

### DEC-018 — Device `IClock` implementation: 64-bit `esp_timer_get_time()` adapter **[DECIDED, owner 2026-10-01; implement at M1/F01]**
- Problem: `IClock` (monotonic `TimeMs` = `uint64_t` ms, `01` §4) is defined and host-tested, but no spec claimed its device implementation (F00 §13 `[OPEN]`). Naive `millis()` is 32-bit and wraps at ~49.7 days, after which every elapsed-time comparison silently reports ~4.3 billion ms.
- Alternatives: (simplest/scalable) an adapter returning `esp_timer_get_time()` (int64 µs since boot) ÷ 1000; (b) `millis()` + a hook accumulating 2³²-ms overflows; (c) a FreeRTOS tick hook accumulating uint64 ticks.
- Trade-offs: (b) and (c) add a hook that must never be missed — one missed overflow/tick is silent wrong time, and tickless/deep-sleep handling can pause the tick; (a) needs no hook or config, counts through light sleep, and cannot wrap for ~292 000 years. Per-boot reset is by design (`01` §4: compare only within one `boot_id`). The interface isolates the choice, so a wrong pick costs one small file.
- Choice: (a), implemented as a device-side adapter **outside `bw_core`** (no Arduino in value types) — F01's HAL or `firmware/src`; host keeps `FakeClock`/a `std::chrono` adapter. Assign to F01 or the integrator **before M1**. Recorded with the owner's approval, 2026-10-01 (F00 Validate session; file is not F00-owned).

### DEC-019 — Prototype scope: real-world validation concerns are out of scope **[DECIDED]**
- Problem: specs carried product-level gates (sensor placement validity, ethics/consent procedures, battery-life targets, field measurements, security hardening) that cannot be closed in code, are outside this academic prototype's software-evaluation scope.
- Alternatives: (simplest) keep them as open items; (scalable) declare the scope: academic breadboard prototype, only architecture and code quality matter, hardware variants are configuration.
- Trade-offs: no claims about real-world reliability; thresholds stay `[EXPERIMENTAL]` defaults. Unaffected because they are code design: `DataQuality`/`Insufficient` outputs, hysteresis, fallbacks, fault handling, host tests.
- Choice: scope statement in `00` §1; items closed or simplified (A-06, A-09, A-11, Q4/Q5/Q7, F25 measurement, F27 consent, F28 consent/pseudonymization, F19 passkey).

### DEC-021 — W3 P4 composition: deferred F10 event-sink via attachOutputs re-call; F14 wired in the decision row **[DECIDED 2026-10-13]**
- Problem: W3 had to bind three seams the wave's own contracts disagree on. (a) F10's `attachOutputs(led, buzzer, events)` is documented "before the first tick" and was called at `f10::wire` with a null sink, but F17 §15.5 prescribes calling it again with `f17::sink()`; (b) calling `f17::sink()` *inside* `wire_alert.cpp`'s `wire()` instead would touch the logger before `f17::wire` — and `wire_eventlog.cpp`'s `loggerImpl` binds `ctx.clock/ctx.faults` on FIRST construction only ("later calls ignore the arguments"), so the logger would be permanently clockless; (c) `wire_confirm.h` states F14's slot is the "decision band, F07–F10 row" while `wiring/README.md`'s band diagram lists F14 under services F11–F18.
- Alternatives: (a1, simplest) re-call `f10::attachOutputs(&f01::led(), &f01::buzzer(), f17::sink())` from the composition root's DI block after `f17::wire`; (a2) edit `wire_alert.cpp` to pass the sink at wire time — ruled out by (b) unless the whole f10 wire call moves after f17, which breaks the README's fixed decision row; (a3) give F10 a dedicated late `attachEventSink` — a new F10-owned seam, unnecessary since `attachOutputs` already documents deferred DI ("null until F17", NotReady + counted pending). (c1) place `f14::wire` after `f10::wire` per the feature's own header; (c2) place it in the services band per the README diagram.
- Trade-offs: (a1) re-calls an output setter after AlertTask has ticked; acceptable because no alert level can be raised before f07 publishes a snapshot (policy stays Normal — tested F10 §9), loopTask and AlertTask share core 1 (no concurrent execution; `alert_engine.h:52` is three plain member stores), and F10's pending-FIFO design explicitly tolerates a late sink. (c1) vs (c2) has zero functional difference; the header is the more specific contract, so it wins and the README diagram is recorded as minor doc drift (backlog).
- Choice: (a1) + (c1). Both are one-line composition-root edits; `app.cpp`'s DI comments cite this DEC. No contract changed; F10/F14 code untouched.

---

## 2. Assumptions to confirm (tagged `[ASSUMED]` across docs)
Hardware assumptions are resolved by configuration (hw_config.h) and never block work.

| ID | Assumption | Affects | How to verify |
|---|---|---|---|
| A-01 | OLED = **SH1106** 128×64, I²C 0x3C `[CONFIRMED controller 2026-09-29; address assumed]` | F01, F12 | Controller confirmed by owner; address → I²C scan |
| A-02 | Buzzer is **passive** `[CONFIRMED 2026-09-29]`; HAL still supports active via config flag | F01, F10 | Confirmed by owner |
| A-03 | Flash = 4 MB `[CONFIRMED 2026-09-29]` | F18, F30 | Confirmed by owner |
| A-04 | Battery has 2 wires only — **no voltage divider seen** `[OPEN, bench at F01]`; battery % = "unknown" until proven otherwise | F01, F23, F25 | Bench: is any ADC pin wired to the battery? |
| A-05 | IMU chip on GY-87 is MPU6050 (WHO_AM_I 0x68); clone IDs possible `[OPEN, verify at F01 bring-up]` | F01 | Read WHO_AM_I once code runs |
| A-07 | Buttons active-low with internal pull-ups `[ASSUMED — generic tactile; verify at F01 bench]` | F01, F11 | Bench |
| A-08 | RGB LED polarity unknown → **code must work without it; configurable in HAL, tested at bench** `[OPEN, F01]` | F01 | Bench |
| A-09 | ~~Wrist placement of MAX30102 gives usable PPG only at rest~~ | — | **CLOSED (out of scope, DEC-019).** Quality gating is already architecture (`DataQuality`, `hr_valid`); thresholds stay `[EXPERIMENTAL]` config |
| A-10 | ESP32-CAM has an SD slot | F27 | Configuration flag; F27 refuses to record without storage |
| A-11 | ~~Phone available to set time~~ | F15, F24 | **CLOSED by design:** unknown wall time is a handled state (DEC-009) |

## 3. Open questions for the developer
_Status: answered by the owner 2026-09-29 (integrator)._

1. **DEC-005** companion stack — **ANSWERED**: no full app now; a **very simple, minimal companion** (Flutter, local-first, no server) is accepted *later*, because DEC-009 needs something to set the watch's time. Nothing fancy.
2. **DEC-001** (PlatformIO) — **ANSWERED**: confirmed; PlatformIO is already installed in VS Code. Proceed with PlatformIO + Arduino-ESP32.
3. Hardware facts A-01…A-08 — **ANSWERED 2026-09-29** (hardware list recreated as `materiel.txt`): A-01 SH1106 ✓ · A-02 passive ✓ · A-03 4 MB ✓ · A-04 no divider visible → battery % stays "unknown" until F01 bench · A-05 `WHO_AM_I` verified at bring-up · A-07/A-08 polarity verified at F01 bench (code must work without the LED). **Remaining items block only F01 (M1), not F00.**
4. ~~Ethics/consent process for recorded data~~ **Closed: out of scope (DEC-019).**
5. ~~Wall-clock time from the phone~~ **Closed: DEC-009.**
6. Button mapping (F11 §13) — **ANSWERED**: accepted **as proposed**, plus one addition: on a screen with multiple selectable options, `Small short` **scrolls** one option at a time; pressing again after the last option advances to the next screen. Recorded in `F11_user_interaction.md` §13.
7. ~~Loudest buzzer pattern for the nap wake alert~~ **Closed: out of scope (DEC-019).**

## 4. Changes versus the original document
| Original | Now | Why |
|---|---|---|
| Django conventions | Django not used (DEC-010) | Local-first; no server needed |
| One list of "states" | Independent axes (DEC-004) | Avoids invalid combinations |
| 30 features, no foundation | **F00 added** | Parallel work needs contracts-as-code first |
| "Alert" as a mode | Alert is an overlay owned by F10 | Alert is transient, mode is a context |
| Low-power as a mode | PowerProfile orthogonal to mode | Night + low battery must be legal |
| F09 as a detector | F09 = experimental detector + data-capture tool | No labeled data exists; capture-first |
| F28 later | F28 serial capture moved to M1 | Every ML feature needs data early |
| Implicit safety scope | Explicit safety/privacy statement | Health data + alert reliance |
