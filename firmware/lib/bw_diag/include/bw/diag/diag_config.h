#pragma once

// bw/diag/diag_config.h — F23 §8 (named constants, every value tagged and
// mirrored in the spec table) and §15.1 DiagConfig. Compile-time defaults;
// DiagConfig carries them as fields so the host suite and a future F18
// settings registration share one path (F04 wear_config.h precedent).
//
// Fault-code boundaries are pinned by fault.h's own comments (F23 §15.9 D9):
// LowBattery fires only when percent < kLowBatteryPercent, CriticalBattery
// only when percent < kCriticalBatteryPercent, TaskOverrun only when
// lateness > kMaxTaskLatenessMs — strict inequalities, tested at the edge.

#include <cstddef>
#include <cstdint>

namespace bw::diag {

// ---- stuck / flat-line (spec §8, D2, D6) ----------------------------------

// [EXPERIMENTAL] 5 s of bit-identical raw IMU readings (250 samples at
// F01's 50 Hz sensor task) is not physical: even a healthy MPU6050
// produces LSB noise. Tune with F28 captures.
constexpr uint32_t kStuckSamples = 250;

// [DERIVED] Identical-run length on 1 Hz MotionFeatures frames: 5 x 1 s
// frames cover the same 5 s as kStuckSamples (250 @ 50 Hz); 1 Hz frames
// are F23's only legal IMU/PPG stream at M2 (§15 D6). The pipeline is
// deterministic, so bit-identical frames (all fields except t_ms) mean
// bit-identical raw input.
constexpr uint8_t kStuckFeatureWindows = 5;

// [EXPERIMENTAL] 2.5 s of bit-identical raw IR counts at F01's 100 Hz
// PPG rate cannot be a pulsatile perfusion signal (shorter than the IMU
// constant because PPG is sampled 2x faster). Tune with F28 captures.
constexpr uint32_t kPpgFlatlineRawSamples = 250;

// ---- PPG contact / saturation (spec §15.3, D6) -----------------------------

// [EXPERIMENTAL] Saturated samples >= 2 % of a raw window (MAX30102
// overflow flag) => PpgSaturated; mirrors F02's gate value (F23 may not
// include F02 headers, so the value is copied with rationale).
constexpr float kSaturatedWindowFraction = 0.02f;

// [EXPERIMENTAL] AC/DC below 0.2 % => contact/perfusion too poor to
// trust the PPG; wider warning band than F02's HR-invalid 0.1 % (which
// sits at 0.001 in F04's evidence floor too, so that band degrades
// instead of claiming a flat-line). Tune with F28/F29.
constexpr float kPoorContactPerfusionIndex = 0.002f;

// ---- freshness (spec §8, §15.4) --------------------------------------------

// [EXPERIMENTAL] 5 missed 1 Hz windows => source Insufficient (one late
// window tolerated; fail-safe, not a guess).
constexpr uint32_t kFeatureStalenessMs = 5000;

// ---- hysteresis (spec D7) --------------------------------------------------

// [EXPERIMENTAL] 2 consecutive bad windows before Degraded; one artifact
// window must not flap quality. Integrator-pinned at G1 (plan D7).
constexpr uint8_t kDegradedEnterWindows = 2;

// [EXPERIMENTAL] 3 consecutive good windows before returning to Good;
// recovery slower than entry (gating errs toward "no reliable data").
// Integrator-pinned at G1 (plan D7).
constexpr uint8_t kGoodRecoverWindows = 3;

// ---- system health (spec §8) -----------------------------------------------

// [USER] Common Li-Po warning level: below 20 % the user should charge
// soon; F12 surfaces it, F23 only diagnoses it. Runtime-tunable via F18.
constexpr float kLowBatteryPercent = 20.0f;

// [USER] Below 8 % there is little headroom left for graceful shutdown and
// event logging (F26's orderly-persistence hook, M5). Runtime-tunable.
constexpr float kCriticalBatteryPercent = 8.0f;

// [EXPERIMENTAL] Task lateness (worst instrumented wake overshoot) above
// 2.5 IMU periods (20 ms) means the processing pipeline is overrunning:
// 2.5x a deadline is the classic overload signal before backlog compounds.
constexpr uint16_t kMaxTaskLatenessMs = 50;

// [EXPERIMENTAL] Free heap below this => LowHeap; 16 KB ~ 3 % of SRAM
// held for bursts/BLE bring-up. Diagnostic threshold, not a crash
// threshold (F26 owns the system response); re-measure at M5.
constexpr uint32_t kMinFreeHeapBytes = 16384;

// ---- fault reporting (spec D9) ---------------------------------------------

// [EXPERIMENTAL] Rate limit: a flapping condition re-reports at most
// once per minute (D9); fault_bits keeps the condition visible meanwhile.
constexpr uint32_t kFaultRepeatMs = 60000;

// [DERIVED] Upper bound on edges one tick can produce. The plan's first
// draft said 4, counting battery as one condition — but §15.3 fires
// CriticalBattery AND LowBattery as two codes from the same 7.9 % tick,
// and two source faults (stuck IMU + saturated PPG) can coincide with
// the health ones: M2 worst case was 6 (1 IMU + 1 PPG + 4 health); M5
// adds LowStack (§16.3) to the health side ⇒ worst is now 7. With a cap
// of 4 a transition edge would be dropped entirely (edges are
// transition-triggered, not queued), so 8 covers the real worst case
// with headroom. [ASSUMED-corrected in IMPLEMENT; flagged in §15 status]
constexpr uint8_t kMaxFaultEdgesPerTick = 8;

// ---- fault-code range (D8) -------------------------------------------------

// [DERIVED] F23's codes are the contiguous run ImuStuckSensor(11) ..
// CriticalBattery(18) in fault.h; fault_bits bit N = code value N, so the
// rate-limit and sticky tables index by code - kF23FaultCodeFirst.
constexpr uint8_t kF23FaultCodeFirst = 11;
constexpr uint8_t kF23FaultCodeLast = 18;
constexpr uint8_t kF23FaultCodeCount = kF23FaultCodeLast - kF23FaultCodeFirst + 1;

// [DERIVED] HealthSnapshot.fault_bits is a uint32_t mask (interfaces/
// services.md §5), so only code values 0..31 are representable.
// Diagnostics range-checks at this width BEFORE the shift: a shift of
// >= 32 is undefined behavior, so an out-of-range/bogus code must be a
// defined no-op instead.
constexpr uint8_t kFaultBitsWidth = 32;

// ---- M5 additions (spec §8 "M5 additions", plan §16) -------------------------

// [DERIVED] W6 P4: the original 12 slots plus independently scheduled wear,
// wake, sleep, cataplexy, event-log, baseline and optional research pumps.
constexpr uint8_t kMaxTrackedTasks = 19;

// [DERIVED] DiagTask's original registry slot (append-only keys);
// DiagTask reports into it each tick, replacing the M2 file-local
// self-lateness counter (§16.1).
constexpr uint8_t kDiagSelfTaskSlot = 11;

// [DERIVED] 1 s of IMU raw samples at F01's 50 Hz plus 28 % jitter
// headroom, mirroring F02's imu_samples queue sizing rule (app.h): a
// slightly-late DiagTask tick must not fabricate a data gap (§8 M5).
constexpr size_t kDiagRawQueueImuSamples = 64;

// [DERIVED] Same sizing rule at F01's 100 Hz PPG rate (app.h ppg_samples
// precedent), so one tick of PPG fits without a hole (§8 M5).
constexpr size_t kDiagRawQueuePpgSamples = 128;

// [EXPERIMENTAL] A valid registry slot whose stack watermark falls below
// this ⇒ LowStack (CR-032): 512 B ≈ FreeRTOS TCB + ISR-nesting worst case
// on the smallest task stack (2048 B). Tune with device high-water marks
// at DoD (§8 M5).
constexpr uint16_t kMinTaskStackFreeBytes = 512;

// [DERIVED] Mirror of bw/fault.h FaultCode::LowStack (CR-032, value 26):
// the range tables below only cover the contiguous 11..18 run, so the
// composer needs the LowStack value spelled out for its D9 rate-limit
// slot (static_assert pins it to the enum in system_health_monitor.cpp).
constexpr uint8_t kLowStackFaultCode = 26;

// [DERIVED] D9 rate-limit table size: F23's 11..18 run (8 slots) plus one
// dedicated slot for LowStack(26) — every F23-owned reportable code gets
// the kFaultRepeatMs flapping guard, foreign codes stay bit-only (I2).
constexpr uint8_t kF23RateLimitSlots = kF23FaultCodeCount + 1;

// ---- config ------------------------------------------------------------------

// The §8 knobs most likely to be tuned first (F28/F29 fixtures, F18
// settings); the remaining thresholds stay plain constants until a
// consumer exists (F04 wear_config.h precedent).
struct DiagConfig {
    uint32_t stuck_samples = kStuckSamples;
    uint8_t stuck_feature_windows = kStuckFeatureWindows;
    uint32_t ppg_flatline_raw_samples = kPpgFlatlineRawSamples;
    float saturated_window_fraction = kSaturatedWindowFraction;
    float poor_contact_perfusion_index = kPoorContactPerfusionIndex;
    uint32_t feature_staleness_ms = kFeatureStalenessMs;
    uint8_t degraded_enter_windows = kDegradedEnterWindows;
    uint8_t good_recover_windows = kGoodRecoverWindows;
    float low_battery_percent = kLowBatteryPercent;
    float critical_battery_percent = kCriticalBatteryPercent;
    uint16_t max_task_lateness_ms = kMaxTaskLatenessMs;
    uint32_t min_free_heap_bytes = kMinFreeHeapBytes;
    uint16_t min_task_stack_free_bytes = kMinTaskStackFreeBytes;
    uint32_t fault_repeat_ms = kFaultRepeatMs;
};

}  // namespace bw::diag
