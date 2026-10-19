# PROJECT CONTEXT — Narcolepsy-Assist Wearable Bracelet

> **Permanent context (core).** Read at the start of every session; sections 2–4, 6, 10–11 are in `context/` and read only when your spec says so. Version 1.0 — 2026-09-29.
> If any feature spec or instruction contradicts this file: **stop and raise it**. Never resolve conflicts silently.
> Status tags used across all docs: **[DECIDED]**, **[ASSUMED]** (confirm with the owner), **[OPEN]** (needs a decision).

---


## 1. Project overview

An embedded-systems engineering project to build a practical wearable bracelet/watch prototype that assists people with narcolepsy by monitoring sleep/wake-related patterns, detecting suspected drowsiness or narcolepsy-related events, and giving timely feedback and alerts.

**It is not intended to diagnose narcolepsy or replace medical diagnosis or treatment.** Purpose: wearable monitoring, event detection, alerting, data collection, personalized assistance.

**Scope.** Academic breadboard prototype, not a product. Only architecture and code quality are evaluated: clean, modular, scalable, modifiable. Real-world concerns (sensor placement, clinical validity, ethics procedures, battery-life targets, enclosure) are out of scope (DEC-019). Detections are worded "possible"/"suspected" and carry a confidence value; the device is not a medical device.

**Data.** Local-first: no cloud, no server. BLE uses standard bonding (F19).

### Engineering expectations
- PPG from a MAX30102 (red+IR only) is weak and motion-sensitive. **Signal-quality gating is mandatory** and "no reliable data" is a first-class output (`DataQuality`, `valid` flags). This is an architectural requirement, not a field-validation task.
- Detecting cataplexy-like events from IMU + PPG is **exploratory research**. F09 is an experimental candidate detector plus data-capture tool, not a validated classifier.
- No labeled narcolepsy dataset exists at project start. The ML path is: **collect data → heuristic baseline → simple trained model → evaluate → iterate** (see F28, F05, F06, F29).

---

## 5. Core design philosophy

- **Probabilities + deterministic rules.** ML outputs probabilities/confidence, never unexplained binary decisions. ML is always combined with deterministic software: temporal smoothing, duration thresholds, hysteresis, contextual rules, sensor-quality checks, state transitions.
- **Multimodal and personalized.** Motion, cardiovascular/PPG, time of day, recent history and the user's baseline all contribute.
- **Graceful degradation.** Sensor loss reduces capability; it never crashes the device (F26).
- **Camera is optional.** Not part of core detection (F27).
- **Explainable.** Important detections carry compact reason codes (F22).
- **Replaceable parts.** Sensors, models, alert types, clients, storage backends and hardware revisions can be swapped behind stable interfaces.

---

## 7. Engineering principles and constraints

Strict separation of concerns · highly modular · scalable · **readability before optimization** · idiomatic conventions per platform · self-documenting code.

### 7.1 "X must not Y" rules
| Module | Must not |
|---|---|
| Sensor drivers | contain ML, alert, UI or business logic |
| Signal processing | contain UI logic or depend on the ML model |
| ML layer | control hardware, or decide alerts |
| Alert system | acquire sensor data |
| Communication (BLE/sync) | contain application/decision logic; reach into internals (uses read-only providers + command queue) |
| Configuration | be mixed into behavior code |
| Data models | be mixed with transport or presentation |
| Any feature | include another feature's headers/concrete classes — only `bw_core` contracts |
| Any feature | edit files it does not own |

### 7.2 Firmware (C++17, Arduino-ESP32 core on PlatformIO — see DEC-001/002)
- Hardware access isolated behind interfaces (F01). No giant `.ino`; `main.cpp` only wires modules (composition root).
- Pure logic modules expose `Output step(const Input&)`-style APIs with **no FreeRTOS/Arduino calls**, so they run in host unit tests.
- No blocking `delay()` in tasks; use `vTaskDelayUntil`. Timing (rates, deadlines, timeouts) is explicit in named constants.
- Static allocation only after startup; fixed-size buffers; no `String`, no exceptions/RTTI; errors returned as `Err`/`Result`.
- Handle sensor disconnect and I²C failure everywhere; supervise with task watchdog (F26).
- Memory/CPU budget declared per feature and checked at Definition of Done.

### 7.3 Python (ML, tooling)
PEP 8, `ruff`, type hints where they add clarity, explicit error handling, `pytest`, no cleverness. Reproducible training (fixed seeds, versioned data + feature schema).

### 7.4 Companion app
Stack **[OPEN — DEC-005; proposed Flutter, local-first SQLite, no server]**. Regardless of stack: layered (transport → repository → presentation), models separate from transport and UI, official conventions of the chosen framework, no unnecessary frameworks. **Django is not used.**

### 7.5 Self-documenting code and "no magic"
Names explain intent; functions do one thing; comments explain **why**. Never: unexplained constants, opaque one-liners, arbitrary thresholds, undocumented library use, silent assumptions.

Every threshold, timeout, rate, ML parameter and hardware behavior is a named constant carrying one tag:

```cpp
// [EXPERIMENTAL] Initial guess; tune with recorded data (F29).
// Why: a window under ~8 s holds too few beats at 60 bpm for a stable estimate.
constexpr uint32_t kPpgHrWindowMs = 10'000;
```
| Tag | Meaning |
|---|---|
| `[HW]` | From a datasheet/board fact (cite section/register) |
| `[DERIVED]` | Computed from stated reasoning (show the reasoning) |
| `[EXPERIMENTAL]` | Guess pending data; will be tuned |
| `[USER]` | Runtime-configurable via settings (F18) |

---

## 8. Design decision rules

For each significant decision state: (1) the problem, (2) alternatives, (3) trade-offs, (4) why the choice fits **this** project. When several solutions exist, always present **the simplest** and **the most professional/scalable**, then choose one and say why. Do not over-engineer; do not take a shortcut with obvious later architectural cost. Balance: simplicity + maintainability + scalability + reliability + embedded constraints. Cross-feature decisions are logged in `04_DECISIONS.md`; feature-local ones in the feature spec.

---

## 9. Development philosophy

Incremental: **Define → Design → Implement → Test → Validate → Integrate.** Each feature is an independent engineering unit, implementable and testable alone. Prefer small traceable changes to rewrites; preserve existing behavior unless the change explicitly requires otherwise. Development and test guidance: `DEVELOPMENT.md`.

---

## Where the rest of this context lives (open only if your spec says so)
| Sections | File | Needed by |
|---|---|---|
| §2 scope, §3 hardware platform | `context/hardware.md` | F01, F23, F25, F27 |
| §4 pipeline & where it runs, §6 state model | `context/pipeline_and_state.md` | F03, F04, F06–F10, F13–F15, F22–F24 |
| §10 feature index, §11 glossary | `context/feature_index.md` | integrator, planning |

