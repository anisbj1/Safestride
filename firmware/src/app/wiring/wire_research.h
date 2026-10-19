#pragma once

// firmware/src/app/wiring/wire_research.h - F28's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_research.* per F28 spec §7
// Owns).
//
// Construction, dependency injection and task table only, no behavior
// (01 §1 rule 3). NOT called from app.cpp yet: adding the call is developer
// action A2 (spec §15); until then the file still compiles into the esp32dev
// build, so A2 stays a one-line change.

#include <cstdint>

#include <bw/research/raw_tee.h>

namespace app {
struct AppContext;
}

namespace f28 {

// Task table (wiring README §3, same shape as wire_f02.h / wire_hal.h).

// [DERIVED] Task period = one streamer poll + format + UART drain cycle per
// tick; alias of bw::research::kResearchTaskPeriodMs (10 ms — its why carries
// both requirements: 12.8 KB/s drain capacity ≥ D2 worst case, and a poll
// rate 100x the 1000 ms feature rate). See research_config.h (F1 root cause).
constexpr uint32_t kResearchTaskPeriodMs = bw::research::kResearchTaskPeriodMs;

// [EXPERIMENTAL] Stack: the streamer object lives in static storage, so the
// stack only carries the task's own frames - drain chunk (128 B), line buffer
// (256 B), formatter scratch (257 B), event baseline batch (8 x 48 B) plus
// call frames. 4096 leaves ~2x headroom over the ~1.5 KB peak estimate; tune
// with a high-water mark if RAM ever matters.
constexpr uint32_t kResearchTaskStackBytes = 4096;

// [DERIVED] Below every pipeline task (SensorTask 5, ProcessingTask 4, F01
// D15): research capture is diagnostics and must never delay the pipeline.
constexpr uint32_t kResearchTaskPriority = 2;

// [DERIVED] Core 0 hosts UART/BLE/flash; keeping capture off core 1 protects
// the sensor + processing deadline path (runtime.md §3.2).
constexpr uint32_t kResearchTaskCore = 0;

// Builds the F28 graph: the static ResearchStreamer (default gate state:
// kResearchCaptureDefaultEnabled, §5/D4) with its sources from AppContext, and
// - only when the capture build-flag is ON - creates ResearchTask.
// With the repo default (flag OFF) this is a no-op: no task, no UART change,
// no rows - byte-for-byte the pre-F28 firmware (spec §15 rollback).
// Idempotence contract: call once, in the wiring README's fixed call order.
void wire(app::AppContext& ctx);

// ResearchTask body (declared per wiring README, defined for device builds):
// one poll/format/drain cycle per kResearchTaskPeriodMs (see wire_research.cpp).
void researchTask(void* arg);

// Hook for the producer-side one-liner (spec §15 A1): the F01 SensorTask
// pushes copies of its samples into this F28-owned tee. Returns nullptr while
// capture is off (§5), so the caller's off-path is one null check and the
// single-consumer contract of ctx.imu_samples / ctx.ppg_samples is untouched
// (D5 - the tee is a separate queue, popped only by ResearchTask).
bw::research::RawTee* rawTee();

}  // namespace f28
