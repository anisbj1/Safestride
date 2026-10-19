# 02 — INTERFACES · data contracts: quality, processing outputs, context estimates

> Part of the shared contracts. **Locked: change only via change request.** Section numbers match the original `02` numbering; index in `docs/02_INTERFACES.md`.

## 4. Data contracts


### 4.1 Quality
```cpp
enum class DataQuality : uint8_t { Good, Degraded, Insufficient };
struct SourceQuality { DataQuality imu; DataQuality ppg; };
```

### 4.2 Processing outputs (F02 → all)
```cpp
struct MotionFeatures {                       // one per 1 s window (rate [EXPERIMENTAL])
  TimeMs t_ms; DataQuality quality;
  float enmo_g;              // mean of max(|a|-1g, 0): standard actigraphy motion metric
  float mag_std_g;           // std of |a|
  float jerk_rms_g_per_s;
  float gyro_rms_dps;
  float gravity_tilt_deg;    // angle of gravity vector to the device z-axis (orientation proxy)
  float dominant_freq_hz;    // 0 if none; used by activity classifier
  uint32_t stillness_ms;     // continuous time below motion threshold
};
struct PpgFeatures {                          // one per 1 s
  TimeMs t_ms; DataQuality quality;
  bool  hr_valid; float hr_bpm; float hr_confidence;
  float perfusion_index;     // AC/DC of IR
  float ir_dc_level;         // raw DC level (used by wear detection)
  float sqi;                 // 0..1 signal-quality index
  bool  motion_corrupted;    // IMU says the window was too energetic to trust
  bool  hrv_valid; float rmssd_ms;   // optional, may stay invalid in v1
};
```

### 4.3 Context estimates
```cpp
enum class ActivityContext : uint8_t { Unknown, Stationary, Sitting, Standing, Walking, Running, LyingResting, InactiveProlonged };
struct ActivityEstimate { TimeMs t_ms; ActivityContext context; float confidence; };
enum class WearState : uint8_t { Uncertain, Worn, NotWorn };
struct WearEstimate { TimeMs t_ms; WearState state; float confidence; };
```
> The classifier must not equate low movement with sleep (context §F03). `Sitting/Standing/LyingResting` may be reported with low confidence or collapsed to `Stationary` when the wrist alone cannot decide.
