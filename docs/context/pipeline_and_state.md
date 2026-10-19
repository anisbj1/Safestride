# 00 — CONTEXT · pipeline and state model (§4, §6)

> Part of `00_PROJECT_CONTEXT.md`; locked.

## 4. High-level pipeline and where it runs

```
Sensor acquisition → Signal processing → Feature extraction → ML inference → State engine
   → Alert engine → User feedback → Event logging → Personalization → Mobile synchronization
```

| Runs on the ESP32 (works with no phone/internet) | Runs on the companion (phone / PC) |
|---|---|
| Sensing, processing, inference, state detection, immediate alerts, user interaction, local event storage, minimal history | Long-term visualization, configuration, analytics, history, sync, data export, model/dataset tooling |

**Local-first rule:** real-time interactions are handled on the ESP32. The wearable must remain useful without a phone. Synchronization is never a precondition for alerting.

**Minimum viable system:** the *local alert loop* — sense → process → estimate → state → alert → button acknowledge → OLED/LED/buzzer — running fully offline. Everything else builds on it.

---

## 6. State model (v0.1 — refined only through change requests)

The original single list of "states" mixed different concepts. It is split into **independent axes**; the State Engine (F07) composes them.

| Axis | Values | Produced by |
|---|---|---|
| Wear | Worn, NotWorn, Uncertain | F04 |
| Activity context | Unknown, Stationary, Sitting, Standing, Walking, Running, LyingResting, InactiveProlonged | F03 |
| Data quality | Good, Degraded, Insufficient (per source: IMU, PPG) | F23 |
| Alertness state | Unknown, NotWorn, Awake, Drowsy, HighDrowsinessRisk, PossibleSleep | F07 |
| Event overlay | Suspected events (e.g. cataplexy-like) are **events**, not states | F09 → F17 |
| Operating mode | Day, Night, Nap, Calibration, Research (+ PowerProfile Normal/LowPower) | F24 |

Mapping of the original list: *Awake/Drowsy/High risk/Possible sleep/Unknown/Not worn* → alertness state; *Normal activity/Low activity* → activity context + motion features; *Suspected narcolepsy event* → event record. "Alert" is a **transient overlay owned by F10**, not an operating mode. Low-power is a **profile**, orthogonal to mode (night + low battery is legal).

---
