# 01 — ARCHITECTURE · runtime and time model (§3–4)

> Part of `01_ARCHITECTURE.md`; locked.

## 3. Runtime model (firmware)

### 3.1 Data-passing primitives (in `bw_core`, F00)
| Primitive | Use | Why |
|---|---|---|
| `LatestValue<T>` (mailbox) | "current state" data: `StateSnapshot`, `MotionFeatures`, `WearEstimate` | Readers want the newest value; dropped intermediates are fine; no queue growth |
| `SpscQueue<T,N>` / FreeRTOS queue wrapper | Ordered messages: `ButtonEvent`, `Command`, `Event`, raw sample batches | Order matters, nothing may be lost silently; overflow counted and reported |
| `RingBuffer<T,N>` | Sliding sample windows, raw event windows | Static storage, O(1) push |

*Simplest*: global variables guarded by critical sections. *Scalable*: typed primitives above. **Chosen: typed primitives** — a global-variable design would silently break the "features only talk through contracts" rule and make host tests impossible.

### 3.2 Tasks (proposal; owners tune stack/priority in their wiring)

W6 startup: all 19 application task shells wait on the composition's one-way
release/acquire barrier before reading provider ports or working. The barrier
opens only after startup attachments, creation attempts and actuator setup.
Startup waiting uses a 10 ms paced grid; no startup spin or fabricated heartbeat.
W6 safe mode is detection/publication-only (G1/P4 ruling): the full task graph
remains active; optional-service suppression and a dedicated safe screen are
deferred, not represented as implemented protection.

| Task | Core | Period | Prio | Owner | Notes |
|---|---|---|---|---|---|
| `SensorTask` | 1 | 20 ms (IMU 50 Hz); PPG FIFO drained every ≤100 ms | high | F01 | Only task that touches I²C sensors; display uses the same `I2cBus` mutex |
| `ProcessingTask` | 1 | 1 s windows | med-high | F02 | Emits `MotionFeatures`, `PpgFeatures` |
| `F03Pump` | 1 | 1 s | 3 | F03 | Activity; independent task/heartbeat, legacy key `Intelligence` |
| `F04Pump` | 1 | 1 s | 3 | F04 | Wear; independent task/heartbeat |
| `F06Pump` | 1 | 1 s (30 s epoch) | 3 | F06 | Alertness probabilities; independent task/heartbeat |
| `F07State` | 1 | 1 s | 3 | F07 | State snapshot; legacy key `State` |
| `F08Sleep` | 1 | 1 s | 2 | F08 | Episode tracking; independent task/heartbeat |
| `F09Pump` | 1 | 1 s | 3 | F09 | Possible event candidates; independent task/heartbeat |
| `AlertTask` | 1 | 50 ms | high | F10 | Pattern player must never block; runs even if other tasks are late |
| `InputTask` | 1 | 10 ms | high | F11 | Debounce, gestures → `Command` |
| `UiTask` | 1 | 100 ms | low-med | F12 | Rendering at ≤10 fps; takes I²C mutex briefly |
| `StorageTask` | 0 | event-driven | low | F18 | Flash writes stall other flash users — isolated from real-time tasks |
| `CommsTask` | 0 | event-driven | low-med | F19 | BLE stack lives on core 0; NimBLE action drain + `BleServer` tick only (CR-018) |
| `ServicePump` | 0 | 2 s (F20/F24), 60 s (F16) | 2 | application composition root | 4096 B stack; `vTaskDelayUntil` grid; bounded work only |
| `F17Log` | 1 | 100 ms | 2 | F17 | 3072 B stack; separate event-log heartbeat |
| `F13Pump` | 0 | 1 s | 2 | F13 | 2048 B stack; baseline learning/persistence off real-time core |
| `F23Diag` | 1 | 1 s | 2 | F23 | 3072 B stack; health registry + raw evidence drain |
| `F26Sup` | 0 | 1 s | 3 | F26 | 4096 B stack; only WDT feed, after all provider attachments |
| `ResearchTask` | 1 | 10 ms | 2 | F28 | Optional capture task; pending/unknown when build flag disabled |

Rules: no `delay()` (use `vTaskDelayUntil`); each task reports `TaskHealth` (last run, max lateness, stack high-water mark) to F23; a late task is a diagnostic, not a crash.

W6 P4 adoption: 19 separate F23 slots (including supervisor) and 18 F26
heartbeat slots. F03/F04/F06 and F07/F08/F09 must never share a slot. F26
Tier A includes all ten sensing/decision/input/alert pumps; service tasks are
Tier B (report/degrade, never gate offline alerting). Disabled research stays
pending Tier B, not failed. Storage is event-driven: periodic wake lateness
does not apply (reported as 0); progress and stack watermark are still measured.
All stack sizes above remain configuration/estimates, not measured watermarks.

### 3.3 Timing budgets `[EXPERIMENTAL]`
IMU 50 Hz `[DERIVED]`: human arm-motion energy is mostly below ~20 Hz, so 50 Hz satisfies Nyquist with margin. PPG 100 Hz `[DERIVED]`: 200 bpm = 3.3 Hz, useful waveform harmonics up to ~10 Hz. State decisions 1 Hz, epoch aggregation 30 s (common actigraphy convention, `[DERIVED]` from literature; confirm against data).

### 3.4 Memory & CPU budget `[EXPERIMENTAL]`
Static application RAM ≤ 150 KB (leaves room for the BLE stack and heap headroom on ~520 KB SRAM). Each feature spec declares its own RAM/CPU budget; F23 measures free heap and stack watermarks; DoD checks them.

---

## 4. Time model
No battery-backed RTC `[ASSUMED]`. Two clocks: **monotonic ms since boot** (always available) and **wall clock** (`UnixSeconds`, set by the phone; `0`/`unknown` before first sync). Events record `boot_id` + monotonic time; wall time is reconstructed at sync via `TimeSyncRecord` (see `02_INTERFACES.md` §6). Features needing wall time (F15 naps, day/night mode, baseline by hour) must handle "unknown" explicitly.

---
