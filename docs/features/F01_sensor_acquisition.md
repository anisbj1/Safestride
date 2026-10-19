# F01 — Sensor acquisition & hardware abstraction (HAL)

> Track **A** · Milestone **M1** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Provide reliable drivers behind the `bw_core` hardware interfaces for MAX30102, MPU6050 (GY-87), buttons, RGB LED, buzzer, OLED and battery ADC; arbitrate the shared I²C bus; detect and report basic hardware faults. Drivers expose clean data and status only.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- Physical hardware (I²C, GPIO, ADC, PWM)
- `IClock`

**Outputs (produced contracts)**
- `IImuSensor`, `IPpgSensor`, `IButtonInput`, `IRgbLed`, `IBuzzer`, `IDisplayCanvas`, `IBatteryMonitor` implementations
- `SensorStatus` per device
- `IFaultReporter.report()` calls on faults
- `I2cBus` (internal to `bw_hal`; the only I²C access point)
- `DeviceClock : bw::IClock` — device monotonic clock (D9; resolves F00 §13's "F01 or the integrator" in F01's favor; `unixSeconds()` = 0 until phone sync)

## 4. Dependencies
- `bw_core` interfaces; `IFaultReporter` (may be a no-op fake until F26).

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Contain filtering, features, thresholds for decisions, alerts or UI layout.
- Let any other module touch `Wire` or I²C.
- Block longer than the documented I²C timeout.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Sharing one I²C bus among OLED, PPG and IMU**
- Simplest: Each driver calls `Wire` directly and hopes.
- Professional/scalable: `I2cBus` class owning `Wire` with a mutex, per-transaction timeout, retry, and stuck-bus recovery (9 SCL pulses).
- Trade-offs: Adds a class but prevents corrupted transactions when the UI task draws while the sensor task reads.
- **Choice:** `I2cBus` — a display flush is ~1 KB at 400 kHz ≈ 25 ms of bus time `[DERIVED]` and would otherwise starve sensor reads.

**D2. MPU6050 read strategy**
- Simplest: Poll the sample registers at 50 Hz in `SensorTask` and stamp with `nowMs()`.
- Professional/scalable: Use the on-chip FIFO with burst reads and reconstruct timestamps from the sample rate.
- Trade-offs: Polling jitters with task lateness; FIFO adds parsing and overflow handling.
- **Choice:** Start with polling (readability); keep the driver interface unchanged so FIFO can replace it if jitter measurements (F23) show a need.

**D3. Display library**
- Simplest: Adafruit_SSD1306.
- Professional/scalable: U8g2 (SSD1306 and SH1106 supported).
- Trade-offs: U8g2 is larger; Adafruit is simpler but breaks if the panel is SH1106 (common clone).
- **Choice:** U8g2, hidden behind `IDisplayCanvas` (DEC-015).

**D4. Buzzer type unknown**
- Simplest: Assume active buzzer on a GPIO.
- Professional/scalable: Config flag: active → GPIO on/off; passive → LEDC PWM at `freq_hz`.
- Trade-offs: Tiny extra code for hardware-agnostic behavior.
- **Choice:** Config flag (A-02).

**D6. I²C seam between policy and platform**
- Simplest: `I2cBus` calls `Wire` directly; host tests fake an Arduino-`TwoWire`-shaped object. Trade-off: the fake must mimic the Arduino API (including its timeout behavior), tests couple to API details, and policy/mechanism blur.
- Professional/scalable: narrow `II2cBackend` (lock/unlock/transfer/recover); all policy (timeout budget, retry, recovery escalation, fault edges) is pure in `I2cBus`; all mechanism (Wire, FreeRTOS mutex, 9-SCL pulse recovery) is one ESP32 file behind the interface.
- **Choice:** `II2cBackend` — matches "hardware access isolated behind interfaces" and lets the §9 test plan drive the policy with a scripted fake.

**D8. Sensor failure counting**
- Simplest: copy a 3-line consecutive-failure counter into each of the 8 drivers. Trade-off: divergence and duplicated tests as drivers evolve.
- Professional/scalable: a shared helper; in `bw_core` it would need a contract change (not justified — F01-local need).
- **Choice:** pure `SensorStatusTracker` **inside `bw_hal`** (feature-local): it decides state transitions only; the driver maps the transition edge to its own `FaultCode` and reports once. `Degraded` is unused by the tracker for now (quality downgrades are F23's model).

**D10. Which drivers are host-testable (purity split inside `bw_hal`)**
- Problem: spec §9 wants driver *behaviour* tested on the host, but the previous session's plan grouped every remaining driver under "platform files behind `#if defined(ARDUINO)`".
- Simplest: guard every new driver whole-file (D5) → bench-only testing; every protocol regression needs hardware.
- Professional/scalable: classify by dependency — a driver whose only collaborators are pure (`I2cBus`, `IClock`, `SensorStatusTracker`, parser/convert) stays unguarded and runs against a scripted register device; only mechanism files (Wire, GPIO, LEDC, ADC, U8g2) get `ARDUINO` guards.
- **Choice:** classification. `Mpu6050Imu` and `Max30102Ppg` are pure (19 host tests cover begin/absent/ID-mismatch, failure counting, FIFO drain/cap/overflow); `Esp32I2cBackend`, `GpioButtons`, `LedcRgbLed`, `Buzzer`, `U8g2Canvas`, `AdcBatteryMonitor`, `DeviceClock` remain platform-only. Trade-off: pure drivers need injected `IClock*`/`IFaultReporter*` — the same nullable pattern `I2cBus` already established (D7).

**D11. PPG overflow and capped-drain correctness**
- Problem: FIFO overrun must surface as `Err::Overflow` (CR-004) without fault spam (D7), and `read(max_samples < available)` must not corrupt timestamps.
- Simplest: report every overrun; stamp the drained subset as if it were the whole stream. Rejected: during a dead drain loop that reports every 100 ms (the exact D7 problem), and a capped read would stamp kept samples up to `(count−n)·10 ms` late — wrong for F02's beat timing.
- Most scalable: time-window suppression owned by F26 and a parser API with global indexing. Not justified now.
- **Choice (feature-local, in `Max30102Ppg::read`):** `OVF_COUNTER > 0` → clear the FIFO, return `Err::Overflow`, and report **one `PpgFifoOverrun` per overrun episode** (re-armed by the next Ok read — same edge rule as D7); the overrun still counts toward `kFailuresBeforeFailed`, so 5 consecutive overruns reach `Failed` + `PpgReadFailed`. A capped drain reads only `n` samples (the rest stays in the FIFO) but anchors the parser at `newest − (count−n)·kPpgSampleIntervalMs`, which makes `t_i = newest − (count−1−i)·dt` exact — both properties are host-proved (`test_ppg_read_caps_but_keeps_absolute_timestamps`).

  **D11 amendment, 2026-10-13 (FIX-A — hardware refutation; evidence `anarolepsy-captures/ppg_first_light_20261013.md`):** the bench proved the counting half of D11 wrong twice over. (a) An ESP32 EN/serial reset does **not** power-cycle the MAX30102: the chip keeps sampling through the whole bootloader window, so at every boot its FIFO is full and `OVF_PTR > 0` before the new firmware's first drain — and on this silicon the FIFO-clear register write does **not** zero the overflow counter (zero write ACKed, counter unchanged, still >0 after 45 s). (b) Counting every overrun toward `kFailuresBeforeFailed` therefore latched `ppg=Failed` permanently at t≈461 ms from that single boot-time episode: the 5 s-paced retry always saw the same stuck counter, so `readOk()` never fired. **New rule:** `begin()` performs a **soft reset** (MODE_CONFIG bit 6, bounded poll `kPpgSoftResetPollAttempts`) right after the part-ID probe — a chip-level reset is the only clear guaranteed regardless of `OVF_PTR` writability, and it also wipes any other stale state inherited across the sensor's unpowered ESP32 resets; and in `read()` an overrun drain is a **stream gap, not a device failure**: `Err::Overflow` + one `PpgFifoOverrun` per episode, counted toward `Failed` **only when consecutive** (`kPpgOverrunStreakBeforeFailure`: after a successful clear the next drain ≤100 ms later cannot see a *new* overrun — refilling 32 samples takes 320 ms at 100 Hz — so a consecutive pair proves the clear did not take, a device-level fault). Trade-off: a hypothetical chip whose counter never clears now reaches `Failed` on the 6th consecutive overrun drain instead of the 5th, and recovers via a clean read (replug/`reinit()`) exactly like any other failure episode — no permanent latch from an episodic condition, no silent Ok-but-empty stream from a permanent one.

  **Second amendment, 2026-10-13 (FIX-A loop 2 — hardware refutation of the amendment above; same evidence log, 65 s soak):** the soak proved the "consecutive drains" rule itself wrong on this clone: because the clear write is a permanent no-op, *every* drain saw `ovf > 0`, so the rule latched `Failed` at exactly the 6th drain (458 ms) — the same latch, one rule later. Root mechanism: an `ovf > 0` drain returned **without popping `FIFO_DATA`**, so the FIFO could never drain and the counter could never even in principle return to 0. **Superseded by D17:** overrun handling is now readback-verified (never trust a clear that was not read back), the data path is never gated on a counter that a pop can work around, a persistent counter gets the bounded reinit ladder before any `Failed` counting, and `kPpgOverrunStreakBeforeFailure` is removed.

**D13. Classifying read failures the Arduino API cannot distinguish**
- Problem: sensor reads use the repeated-start pattern (`endTransmission(false)` + `requestFrom`). Verified against arduino-esp32 2.0.17 source: the deferred write means `endTransmission(false)` *never transmits and always returns 0* (Wire.cpp: it only marks `nonStop`; the bytes go out inside `requestFrom`), and `requestFrom` collapses **every** error — address NACK and timeout alike — into "0 bytes" (`i2cWriteReadNonStop` sets readCount = 0). `I2cBus`' D1/D7 policy needs `NotFound` (device absent → no recovery, no bus fault) vs `Timeout` (bus trouble → one report + 9-SCL recovery). Mapping 0 bytes → `Timeout` would pulse and report `I2cBusStuck` for a merely-unplugged sensor — the exact misclassification D1 forbids.
- Simplest: map 0 bytes → `Timeout`. Rejected: absence becomes a bus fault (D1: "a missing device is not a bus fault").
- Professional/scalable: split into a STOP'd write (full error mapping) + a separate read — loses the repeated start, which both datasheets' register-read flows use (support for a STOP between pointer write and read is not documented); or patch U8g2/core upstream — not maintainable.
- **Choice:** an address-only probe **on the failure path only**: after `got == 0`, a zero-length `endTransmission(true)` re-asks the bus (the core's own bus-scanner path — `esp32-hal-i2c.c` explicitly implements zero-length writes for scanning): NACK → `NotFound`, ACK → `Timeout` (device answers, the read failed; the core hid the cause — bounded and retryable, keeps D7's outage policy intact), timeout/other → mapped. The pure mapping lives in `bw_hal/i2c_error_map` (host-tested: 8 tests pin the Wire 0–5 contract); the backend only executes it. Trade-off: one extra ~20 µs transaction **per failed read only** — negligible next to a wrong outage classification.

**D14. Routing U8g2 through `I2cBus` without forking the library**
- Problem: U8g2's Arduino byte callback talks to `Wire` directly (forbidden — 00 §3: only F01 touches I²C), and its transfer failures are silent (`sendBuffer()` returns void).
- Simplest: use U8g2's software-I2C pins and accept a second bus owner. Rejected: violates the single-owner rule and bit-bangs the shared bus.
- Professional/scalable: fork U8g2 or carry a patched user-pointer build — maintenance burden for a seam the library already offers for free.
- **Choice:** install a custom `u8x8.byte_cb` after construction, **before** `begin()` (U8g2's own extension point): `INIT`/`SET_DC` declined (bus + clock owned by `Esp32I2cBackend`; the D/C bit is the cad's leading control byte), `SEND` accumulates into `kDisplayTxBufferBytes` (64 B; the ssd13xx fast cad tops out at 1 control + 1 command + 31 args = 33 B), `START_TRANSFER` stores `i2c_address >> 1`, `END_TRANSFER` flushes via `I2cBus::write` and records the `Err` that `flush()` returns. `u8x8_t` has no user pointer on Arduino builds (`U8X8_WITH_USER_PTR` is unix-only) → the cb resolves the single canvas through a file-local pointer; one SH1106 on one bus is a hardware fact (00 §3) and u8g2's own cad already keeps a static `in_transfer` for the same reason. Trade-off: two canvases would collide — impossible while §3 pins exactly one display. Absence = ACK probe in `begin()` (no ID register), `DisplayFailed` one report per absent episode (Warning).

**D16. Bench triage of the accel anomaly: raw register echo + boot-stage WHO_AM_I trail**
- Problem: the orientation test must separate "scale factor wrong" vs "offset" vs "config write never took" vs "reads are garbage", but `ImuSample` (locked contract) carries only converted floats, Serial may not live in F01 (01 §1), and a diagnostic must not disturb the very failure streak and fault counters it is diagnosing.
- Simplest: the composition root reads the registers itself — rejected: a second I²C owner outside `I2cBus` is forbidden (00 §3). Next-simplest: widen `read()`/`ImuSample` with raw counts — rejected: a CR against a locked contract for bench-only data, permanently polluting a clean interface.
- Professional/scalable: a read-only probe on the concrete driver — `Mpu6050Imu::readRawDiagnostics() → Result<Mpu6050Diag>` (raw burst counts, accel offset registers, config echo, WHO_AM_I) with **zero** side effects on `status()`/tracker/faults, plus a WHO_AM_I probe recorded after each bring-up stage in `wire()` and appended by `f01::imuDiag()`; formatting happens only in the composition root. Generalizes: every future "is the chip still answering, and since when?" question is one bench line.
- Trade-offs: ~6 extra read-only bus transactions per bench cycle (<1 ms on a healthy bus), a 200-char static buffer, and code that must be deleted with the scaffolding it serves — accepted because it turned "4.02 g at rest" from speculation into the evidence recorded in the status log (session 8).
- **Choice:** the probe. Host-tested (4 tests pin the register echo against `begin()`'s writes, streak/fault neutrality in both directions, mute-on-absence); the stage trail (`stg=imu:… ppg:… io:… disp:… batt:…`) bisects the first stage whose WHO_AM_I answer differs from the `0x68` that `begin()` proved at boot.

**D17. PPG overrun adaptation: readback-verified clear, pop-consistent fallback, bounded reinit ladder (loop-2 hardware refutation)**
- Problem: the 2026-10-13 loop-2 soak (65 s, COM14) refuted the loop-1 fix on hardware: `PpgFifoOverrun` at 358 ms, `PpgReadFailed` at 458 ms — **exactly 5 drains after the first overrun**, i.e. the loop-1 "consecutive drains count" rule latched `Failed` again, because on this clone **no write ever cleared `OVF_PTR`** (counter >0 on every drain for the whole soak). Mechanism of the death spiral: `read()` saw `ovf > 0` → cleared via the 3-register write (a permanent no-op here) → returned **without popping `FIFO_DATA`** → the FIFO stayed full forever → every subsequent drain saw `ovf > 0`. Two candidate silicon truths had to be handled without assuming either: (a) `OVF_PTR`/pointers not clearable by register writes (observed pre-fix: 45 s of `ovf > 0` despite a zero write every drain); (b) the FIFO re-overrunning before each drain (excluded by arithmetic: the boot overrun lands within the first fill window, and 2 drains/20 ms cannot refill a 32-sample FIFO). Additionally the clone may **ignore the MODE_CONFIG RESET write entirely** (drops it; the bit poll reads "already clear", so a driver that trusts the reset believes a lie).
- Alternatives: (1) keep trusting the clear write and count consecutive overrun drains (loop-1) — refuted by the soak above; (2) once `ovf > 0` persists, permanently stop draining — dead stream, worse; (3) never gate the data path on the counter: make every clear **readback-verified**, fall back to a pop-consistent drain (`FIFO_DATA` reads advance `RD_PTR` by silicon design), and treat a counter that survives both as a *stuck register* to be latched as a documented degradation; (4) replace the driver with a MAX3010x library — rejected: third-party code outside the D10 purity/test seam.
- Trade-offs of (3): ~8 extra boot transactions (readbacks), a settle drain discarding ≤32 stale samples at boot, and — on stuck-counter silicon — overrun *detection* becomes unavailable after the latch (samples keep flowing; F02/F23 still see quality effects, and the fact is printed on the bench line). Accepted: the alternative was a latched-`Failed` bracelet.
- **Choice:** (3), plus: `begin()` ends with a **settle drain** (bounded, boot context, no `delay()` — sampling starts at the mode write but the first task drain is ~100-300 ms out; discarding anything in the FIFO now means that gap cannot overrun the 32-sample window under truth (a) *or* (b), and the FIFO_DATA burst is proven end-to-end in boot context); a persistent `OVF` at runtime gets the **bounded reinitialization ladder** (R2: soft reset + reconfigure + verified clear + mode + settle, `kPpgOvfReinitAttemptsPerEpisode = 2` [EXPERIMENTAL], run **once per episode**), and only when even that cannot re-sync the pointers (`WR != RD` after a pop) does the episode count as read failures toward the honest `Failed` state — which the `kSensorAbsentRetryMs` retry recovers from. Every step adapts via **register readback** (`reset_cleared`, `clear_via_write`, `ovf_stuck` latched in `Max30102PpgDiag`, appended to the D16 bench line as `ppg:rst= clr= ovf= ep=`), so the same firmware works under both silicon truths and the bench can tell which one held. No contract change: existing `FaultCode`s only.

## 7. Module structure
- `bw_hal/I2cBus` + `II2cBackend` seam (D6): policy pure, mechanism ESP32-only
- `bw_hal/i2c_error_map` (pure, D13): Arduino `endTransmission`/probe codes → `Err`, host-pinned to the core contract
- `bw_hal/SensorStatusTracker` (D8): shared consecutive-failure state transitions (`kFailuresBeforeFailed` lives here so drivers cannot diverge)
- `bw_hal/DeviceClock : bw::IClock` (D9): monotonic ms from `esp_timer`, platform-only
- `bw_hal/max30102_parser` (pure): FIFO bytes → `PpgSample`, pointer wrap, 18-bit mask, timestamps
- `bw_hal/mpu6050_convert` (pure): `be16`, range → sensitivity table, `ImuRaw` → `ImuSample`
- `bw_hal/Mpu6050Imu : IImuSensor` (pure protocol, D10): WHO_AM_I, wake/config burst, one burst read per call, `readRawDiagnostics()` bench probe (D16, no side effects)
- `bw_hal/Max30102Ppg : IPpgSensor` (pure protocol, D10): part ID, config, FIFO drain, overflow policy (D11)
- `bw_hal/battery_convert` (pure): ADC counts → `BatteryReading`, A-04 "no divider → unknown"
- `Mpu6050Imu` / `Max30102Ppg` / `battery_convert` compile unguarded (D10); everything below is platform-only:
- `GpioButtons : IButtonInput`
- `LedcRgbLed : IRgbLed` (polarity from config)
- `Buzzer : IBuzzer`
- `U8g2Canvas : IDisplayCanvas`
- `AdcBatteryMonitor : IBatteryMonitor` (delegates to `battery_convert`)
- `Esp32I2cBackend : II2cBackend` (Wire + FreeRTOS mutex + 9-SCL recovery; read-failure probe per D13)
- `include/config/hw_pins.h` (single source of pin facts) + `include/config/hw_config.h` (rates, device-type and battery facts)
- `firmware/lib/bw_hal/library.json`: U8g2 dependency (D5 keeps `platformio.ini` integrator-owned)
- `src/app/wiring/wire_hal.*` (D15): static-in-storage HAL graph, `ctx.clock = &DeviceClock`, composition-root accessors, SensorTask constants — construction/injection only, no behavior; bench tooling `i2cScan` / `imuDiag` (D16, incl. the boot-stage WHO_AM_I trail) / `pinFinderStep`, all scaffolding flagged for removal with the D15 task table
- Platform-only files are compiled only under `ARDUINO` (D5); pure modules must build for `native`.

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kI2cClockHz` | 400000 | `HW` | Both MPU6050 and MAX30102 support fast-mode (400 kHz) per their datasheets |
| `kI2cTransactionTimeoutMs` | 20 | `EXPERIMENTAL` | Longer than the longest normal transfer, short enough to keep a 50 Hz sensor loop alive |
| `kImuSampleRateHz` | 50 | `DERIVED` | Human arm-motion content mostly < 20 Hz → Nyquist with margin |
| `kImuAccelRangeG / kImuGyroRangeDps` | ±4 g / ±500 dps | `EXPERIMENTAL` | Covers walking/running arm swing without saturating; finer resolution than ±16 g |
| `kPpgSampleRateHz` | 100 | `DERIVED` | HR ≤ 200 bpm = 3.3 Hz; 100 Hz resolves pulse peaks and limits I²C/power |
| `kPpgFifoPollMaxIntervalMs` | 100 | `DERIVED` | FIFO holds 32 samples = 320 ms at 100 Hz; polling ≤ 100 ms leaves 3× margin |
| `kPpgLedCurrentMa` | ≈ 7 (start) | `EXPERIMENTAL` | Start low to save power; F23 reports saturation/low-amplitude so it can be tuned |
| `kExpectedMpuWhoAmI` | 0x68 | `HW` | MPU6050 WHO_AM_I register 0x75; clones may differ (A-05) |
| `kExpectedMax30102PartId` | 0x15 | `HW` | MAX30102 Part ID register 0xFF |
| `kFailuresBeforeFailed` | 5 consecutive | `EXPERIMENTAL` | Avoids flapping on a single glitch while detecting real disconnects within ~0.1–0.5 s |
| `kI2cLockTimeoutMs` | 20 | `EXPERIMENTAL` | Mutex wait before `Err::Busy`; the lock is held ≤ one transaction, so one transaction period bounds sensor-cycle impact while guaranteeing entry under contention |
| `kI2cTransferAttempts` | 2 | `EXPERIMENTAL` | Two attempts absorb a transient glitch; a single glitch must not escalate to 9-SCL recovery, which disturbs every device on the shared bus |
| `kPpgFifoBytesPerSample` | 6 | `HW` | 3 bytes red + 3 bytes IR per FIFO sample, MSB first (MAX30102 FIFO) |
| `kPpgAdcFullScaleCounts` | 262143 | `HW` | 18-bit ADC full scale = 2^18 − 1; `PpgSample::saturated` when either channel hits it |
| `kPpgFifoDepth` | 32 | `HW` | FIFO holds 32 samples (00 §3); WR/RD pointers are 5-bit registers |
| `kImuDlpfCfg` | 3 | `DERIVED` | 44/42 Hz BW anti-aliases the 50 Hz ODR (Nyquist 25 Hz) and gives the 1 kHz base SMPLRT_DIV math assumes; reset default 0 = 8 kHz bypass would alias (D12); value = developer's reference project, validated on this module 2026-10-08 |
| `kImuSampleRateDiv` | 19 | `DERIVED` | `1000/kImuSampleRateHz − 1` with the DLPF enabled |
| `kPpgLedCurrentPaValue` | 36 | `EXPERIMENTAL` | LED_PA ≈ 0.196 mA/code (0x00–0xFF ≈ 0–50 mA) → ≈ 7.1 mA, the `kPpgLedCurrentMa` start value |
| `kPpgSampleIntervalMs` | 10 | `DERIVED` | `1000/kPpgSampleRateHz`, whole milliseconds so timestamp reconstruction has no rounding residue |
| `kPpgSoftResetPollAttempts` | 64 | `EXPERIMENTAL` | Poll budget for the MODE_CONFIG reset bit in `begin()` (D11 amendment): each poll is one register read (~0.3 ms at 400 kHz incl. lock) → ≈ 20 ms against the typically few-ms reset (community libraries budget 100 ms); one-time at boot inside `wire()`, never in a task loop; a chip that never clears the bit gets `Err::Timeout` and boot continues (§10) |
| `kPpgOvfReinitAttemptsPerEpisode` | 2 | `EXPERIMENTAL` | D17/R2 bounded reinitialization ladder: when the readback-verified FIFO clear fails at runtime (persistent `OVF`, unsynced pointers), begin()'s full post-probe sequence (soft reset + reconfigure + verified clear + mode + settle) is retried at most this many times, on the episode's first drain only (≈15 bus transactions per attempt, ≈5 ms, no `delay()`); after the budget the episode counts as read failures toward the honest `Failed`, which the `kSensorAbsentRetryMs` retry still recovers from. 2 = one attempt absorbs a transient, the second confirms a persistent condition without stalling SensorTask. Tune with bench data |
| `kI2cWireBufferBytes` | 256 | `DERIVED` | Largest single transfer is the PPG FIFO drain `kPpgFifoDepth × kPpgFifoBytesPerSample` = 192 B + 1 B register pointer; the core default of 128 B would truncate it — set **before** `Wire.begin` (buffers allocate there) |
| `kI2cRecoveryPulseCount` / `kI2cRecoveryHalfPeriodUs` | 9 / 10 µs | `HW` / `DERIVED` | Standard bus-recovery practice: 9 clocks shift out the byte a stuck slave holds; 10 µs half-period ≈ 50 kHz is followable by every device, total pulse time 180 µs (inside the transaction budget) |
| `kLedPwmFreqHz` / `kLedPwmResolutionBits` | 5000 / 8 | `DERIVED` | 5 kHz is ≫ flicker fusion and realized exactly at 8 bits (the Arduino-ESP32 ledc example pair); 8 bits = `Rgb`'s channel resolution, no truncation |
| `kBuzzDefaultFreqHz` / `kBuzzPwmResolutionBits` | 2000 / 8 | `EXPERIMENTAL` / `DERIVED` | Fallback for `set(true, 0)` — mid-audible where typical passive piezos are loudest (tune at bench); 8 bits → 50 % duty = 128/256 for maximum square-wave output |
| `kBatteryAdcSampleCount` | 8 | `EXPERIMENTAL` | ESP32 single conversions are noisy/non-linear; averaging 8 cuts random noise ≈ √8 at < 1 ms, and F23 reads ≤ 1 Hz |
| `kDisplayTxBufferBytes` | 64 | `DERIVED` | ssd13xx fast-cad transfer max = 1 control + 1 command + 31 args = 33 B (u8x8_cad.c); overflow is refused (`Err::Full`), never truncated |
| `kSensorTaskPeriodMs` / `kSensorTaskStackBytes` / `kSensorTaskPriority` / `kSensorTaskCore` | 20 ms / 4096 B / 5 / 1 | `DERIVED` / `EXPERIMENTAL` / `HW` | period = 1000/`kImuSampleRateHz`; stack measured on device 2026-10-08: task entry sits 636 B deep and the *failing*-read path (I²C call chain ≈1.1 KB + Wire error log ≈0.7 KB) overflowed 2048 B and smashed the I²C driver's queue below the stack (boot-loop assert, session 11) — 4096 keeps ≥1.5 KB margin; "high" of runtime.md §3.2 mapped to ESP32's 0–25; core 1 exactly per the task table |
| `kSensorAbsentRetryMs` | 5000 ms | `DERIVED` | retry period for a sensor whose `SensorStatus` is `Failed` (reads otherwise run every tick): preserves `readOk()` replug recovery within ≤5 s while cutting the failing I²C transaction + error log from 50 Hz to 0.2 lines/s (session 11) |
| `kImuDiagMsgChars` | 256 | `DERIVED` | worst-case `imuDiag()` payload = 129 chars (raw/gyro/off triples + register fields) + 59 chars (D16 boot-stage trail) + 37 chars (D17 PPG readback facts `ppg:rst= clr= ovf= ep=`) = 225, + headroom so the bench line never truncates (derivation in `wire_hal.h`) |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Host: `I2cBus` logic with a fake `Wire` (timeout, retry counting, recovery trigger); `I2cBus::scan` (collects ACKs, aborts after one outage, respects bounds).
- Host: MAX30102 FIFO parsing from byte fixtures (wrap-around, 18-bit masking, overflow flag).
- Host: MPU6050 raw → g/dps conversion from fixtures.
- Host: `Mpu6050Imu` / `Max30102Ppg` protocol against a scripted register device (D10): begin/ID-mismatch/NACK, config register values, consecutive-failure → `Failed` + one fault per episode, FIFO drain, capped drain with absolute timestamps, `Err::Overflow` + `PpgFifoOverrun` edge (D11). PPG boot/overrun regressions (D11 amendments + D17, 2026-10-13): the scripted device models the 2026-10-13 silicon truth set — the inherited boot state (stale FIFO + `OVF_PTR`), the soft-reset bit (incl. a clone that **drops** the RESET write), non-clearable `OVF_PTR`, ignored pointer writes, and FIFO_DATA reads that never advance `RD_PTR`. Pinned: a boot overrun recovers to `ppg=Ok` with one Warning on both the reset-honoring and the write-ignoring clone (`reset_cleared`/`clear_via_write`/`ovf_stuck` readback facts asserted); `begin()` resets-and-waits before configuring; a stuck reset fails bounded (`Timeout`, exact poll-budget transfer count); an episodic overrun never latches (bounded ladder restores a clearable counter: one `Overflow`, `reinit_runs` counted, detection restored); a mid-session fully-dead chip (nothing honors writes, pops never drain) reaches `Failed` with exactly one `PpgReadFailed`, the ladder budget `kPpgOvfReinitAttemptsPerEpisode` run once per episode, and recovers to `Ok` on the next clean read; a mid-session stuck-counter clone adapts (`ovf_stuck`) and keeps returning real samples with `Ok` forever.
- Host: `Mpu6050Imu::readRawDiagnostics` (D16): register echo equals `begin()`'s writes, a failed probe does not count toward the streak nor report, a successful probe does not reset the streak, absent chip → mute `NotFound` (no recovery, no fault).
- Host: `batteryFromAdc` — divider math, clamps, A-04 absent → `valid = false`.
- Host: `i2c_error_map` — Wire `endTransmission` codes → `Err` and the D13 read-failure classification (probe ACK → `Timeout`, NACK → `NotFound`, never `Ok`).
- Bench (documented checklist): WHO_AM_I/part ID read, sensor unplug/replug, button polarity, LED polarity, buzzer type, battery ADC vs multimeter (3 points).

## 10. Failure behavior
- N consecutive failures → `SensorStatus::Failed` + fault report; `reinit()` available; timing/back-off policy belongs to F26.
- Sensor absent at boot → `begin()` returns `Err::NotFound`; boot continues.
- Display failure never blocks sensing.

## 11. Budget
RAM ≤ 12 KB (U8g2 full buffer 1 KB + FIFO buffers + driver state) `[EXPERIMENTAL]`. CPU: IMU read ≈ 14 bytes/20 ms; PPG drain ≤ 32×6 bytes/100 ms.

## 12. Out of scope
- Magnetometer/barometer use (v1 leaves them idle).
- Power gating (F25).
- Calibration/offset estimation (F02 if needed).

## 13. Open questions / assumptions
- Hardware assumptions A-01..A-08 are configuration in `hw_config.h`; defaults are chosen and none blocks work. The magnetometer sits behind the MPU6050 bypass bus: leave it disabled in v1.
**A-01…A-08 — bench answers (Bench A, 2026-10-01/02; `04` §2 is the index, this is the F01 record):**
- **A-01 (OLED controller)** — **ANSWERED:** SH1106, I²C address **0x3C** `[HW]`. Evidence: `scan=Ok n=1 [3C]` every cycle + "F01 alive" renders clean (row ③). No CR needed.
- **A-02 (buzzer type)** — **ANSWERED:** **passive** `[HW]`; clear 2 kHz tone at boot and every 5 s (row ⑥). `HW_BUZZER_IS_PASSIVE=1` stands; pin 16 verified too.
- **A-03 (flash 4 MB)** — owner-confirmed 2026-09-29 (not F01's bench); esptool reports a 4 MB image layout.
- **A-04 (battery divider)** — **ANSWERED:** **no divider** `[HW]`; `batt valid=0 unknown` (row ⑦), `HW_BATTERY_DIVIDER_PRESENT=0` stands, `BatterySenseUnavailable` (Info) reported once at boot. `HW_PIN_BATTERY_ADC 34` stays `[ASSUMED]` (the pin is deliberately never driven, so the bench cannot verify it).
- **A-05 (WHO_AM_I / part ID)** — **IMU ANSWERED (PASS, Bench B, 2026-10-03):** `scan=Ok n=2 [68 77]` + `imu=Ok` → WHO_AM_I **0x68 = genuine MPU6050**; `0x77` is the GY-87's BMP180 barometer (unused, §12); HMC5883L (0x1E) absent on this clone. **PPG still OPEN:** MAX30102 not fitted (no 0x57 answers) → `ppg=Failed` is the expected absent-device result (§10); re-run the part-ID row when the MH-ET board is connected. **Owner decision 2026-10-03 (session 9):** close F01 with this row **explicitly deferred** — the absent-device result is §10-conformant and the developer has no MAX30102 hardware; re-run when the MH-ET board is fitted. (The accel-at-rest scale anomaly is a separate open defect tracked in the status log — not an A-05 identity issue. **Session 8 update:** the boot-time PASS stands (`begin()` verified `0x68` or `imu` could not be `Ok`), but the D16 register echo now shows WHO_AM_I answering **0x00 at runtime** plus frozen accel counts — see the session-8 status entry before trusting either reading. **Session 10 update (2026-10-08): that caveat is WITHDRAWN** — the D16 anomaly was a firmware bug (missing RX-buffer drain in `Esp32I2cBackend::transfer()`, fixed session 10; hardware never at fault, proven by the developer's reference project on this board). All readings now trustworthy: `stg=…:0/68` ×5, `id=0x68` at runtime, echo **19/03/08/08** (DLPF→3), raw counts track orientation.)
- **A-06 (TP4056 status pins)** - **removed from `04` §2 (DEC-019):** charging status is not required; `BatteryReading::charging_known = false` already reports it as unknown, so it never blocks work. Bench observation kept in the status log.
- **A-07 (button polarity)** — **ANSWERED:** **active-low with internal pull-ups** on **BIG = GPIO26, SMALL = GPIO27** `[HW]` (2026-10-02). Evidence: 12 `[BTN]` edges — 3 big pairs, 2 small pairs, 1 both-press → `led=RED/GREEN/BLUE` matching — plus 3-minute idle windows at `big=0 small=0` with no spurious flips.
- **A-08 (RGB polarity/pins)** — **ANSWERED:** **common-anode, active-low** `[HW]` (`HW_RGB_ACTIVE_LOW=1`, measured 2026-10-01); **anode fed from GPIO25** (driven HIGH in `wire()`), **R = GPIO14, G = GPIO13, B = GPIO4** `[HW]` (2026-10-02 — red/green were swapped vs. the original 13/14 guess). Evidence: full R→G→B rotation by eye + button→LED test colours matching the log.
## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Bring-up checklist executed on hardware and results recorded in the status log.
- `hw_pins.h` holds every pin/polarity fact with a source comment.
- Unplugging any single sensor does not stall other sensors.

---
