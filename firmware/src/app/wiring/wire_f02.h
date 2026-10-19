#pragma once

// firmware/src/app/wiring/wire_f02.h - F02's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_f02.* per F02 spec 2
// Owns).
//
// Construction and dependency injection only, no behavior (01 1 rule 3).
// Not behind `defined(ARDUINO)`: SignalProcessor is pure logic and compiles
// for both esp32dev and native (README 3).

#include <bw/signal/signal_processor.h>

namespace app {
struct AppContext;
}

namespace f02 {

// Constructs the F02 graph: the static SignalProcessor with its compile-time
// configuration (bw/signal/signal_config.h, F02 D9) and — task table landed
// 2026-10-08 (F01 D15) — creates ProcessingTask, which drains the D10 queues
// and publishes the MotionFeatures/PpgFeatures mailboxes.
// Idempotence contract: call once, in the processing slot of the README's
// fixed call order (after f01::wire, before f03::wire).
void wire(app::AppContext& ctx);

// ProcessingTask body (runtime.md §3.2, declared per wiring README): per 1 s
// window tick it drains both AppContext queues into the processor
// (pop → feedImu/feedPpg), calls step(now), and publishes each closed window
// to ctx.motion_features / ctx.ppg_features. Created by wire(); `arg` is the
// AppContext.
void processingTask(void* arg);

// Composition-root accessor: the processor instance is private to
// wire_f02.cpp; ProcessingTask (declared in this pair) reaches it here - never by
// including another feature's headers (01 1 rule 1).
bw::signal::SignalProcessor& processor();

// --- ProcessingTask constants (runtime.md §3.2: F02 owns ProcessingTask) ---
// Task body declared above; constants here keep stack/priority/core/period
// named with reasons (wiring README). Instantiated with the task table
// 2026-10-08 together with F01's SensorTask (F01 D15).

// [DERIVED] One task cadence per feature window: the scheduler inside
// SignalProcessor closes a window when step() runs at/after its end, so the
// period IS the window (F02 D4) - referencing kMotionWindowMs keeps a single
// 1 s source of truth instead of a second literal.
constexpr uint32_t kProcessingTaskPeriodMs = bw::signal::kMotionWindowMs;

// [DERIVED] All window state lives in the static processor (D4 staging
// ~10 KB, not on the stack); the task only moves samples between the two
// AppContext queues and calls step(), so its frames are a few hundred bytes.
// 2026-10-14 (F02 FIX): raised 2048 -> 3072 because the task now formats the
// [PPGWIN] bench line with float conversions (newlib %f takes ~0.5-1 KB of
// stack) — an overflow here would crash, not degrade. Re-measure at DoD with
// uxTaskGetStackHighWaterMark and shrink back if the high-water mark proves
// the margin is never touched.
constexpr uint32_t kProcessingTaskStackBytes = 3072;

// [DERIVED] runtime.md 3.2 ranks the tasks high (SensorTask = 5, wire_hal.h)
// > med-high > med; one step below the sensor task keeps the data path
// ordering of that ladder. Tune at integration when the task table is
// instantiated.
constexpr uint32_t kProcessingTaskPriority = 4;

// [runtime.md 3.2] Core 1 - same core as SensorTask, so samples never cross
// cores between producer and consumer (SPSC queues stay cache-local).
constexpr uint32_t kProcessingTaskCore = 1;

}  // namespace f02
