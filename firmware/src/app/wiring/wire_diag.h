#pragma once

// firmware/src/app/wiring/wire_diag.h — F23's wiring pair (convention:
// src/app/wiring/README.md; the file name wire_diag.* is pinned by F23
// spec §2 Owns).
//
// Construction, dependency injection and task creation only — all
// behavior lives in bw_diag (01 §1 rule 3), which the host suite drives
// with scripted DiagInputs. This pair is NOT behind `defined(ARDUINO)`:
// Diagnostics is pure logic and must compile for both esp32dev and
// native (README §2); only the task body and the f01::/ESP bindings are
// guarded.
//
// Output seam (plan D5, integration-approved at G1): file-static
// LatestValue mailboxes in wire_diag.cpp + the two read accessors below.
// The P4 step wires F07's pump to these accessors; F23 delivers only this
// pair and never edits app.h/app.cpp (F07 attachMailboxes precedent).
//
// M5 additions (plan §16):
//  - D13 per-task registry: kMaxTrackedTasks mailboxes keyed by
//    bw::diag::TaskSlot + noteTaskCycle() (single-writer-per-slot RMW) and
//    the latestTaskHealth() accessor; other tasks adopt it with one P4
//    line each (integrator's call);
//  - D10 attachReliabilityProvider(): F26's CR-030 seam — value copied at
//    this seam each tick, never handed to pure logic as a pointer;
//  - D15 raw tee: onImuSample()/onPpgSample() producer hooks (F01's
//    SensorTask calls them — integrator/P4 item, F28 RawTee pattern)
//    and the gatherInput() drain with the drop-hole rule.

#include <cstdint>

#include <bw/containers/latest_value.h>
#include <bw/diag/diag_types.h>
#include <bw/diag/task_registry.h>
#include <bw/hardware.h>
#include <bw/providers.h>
#include <bw/quality.h>
#include <bw/task_health.h>

namespace app {
struct AppContext;
}

namespace f23 {

// Read SensorTask's published copy; never reach mutable HAL trackers from UI/bench.
bool latestSensorStatuses(bw::SensorStatus& imu, bw::SensorStatus& ppg);

// Constructs the F23 graph (static-storage Diagnostics with the §8
// defaults) and creates DiagTask. Idempotence contract: call once, in the
// "services" slot of the README's fixed call order (after f01::wire — the
// sensor/battery bindings are then valid for the task's whole lifetime).
void wire(app::AppContext& ctx);
void reportTaskCreateFailure(const app::AppContext& ctx, bw::TimeMs now_ms);

// DiagTask body (runtime.md §3.2, declared per wiring README): the
// FreeRTOS shell that paces the 1 Hz tick, gathers the DiagInput
// (D4/§15.4), steps bw::diag::Diagnostics once and publishes. Created by
// wire(); `arg` is the AppContext.
void diagTask(void* arg);

// --- task constants (plan D3, integrator-pinned at G1; M5 §16.1) -----------

// [DERIVED] SourceQuality is produced at 1 Hz (spec §3), so the composer
// runs on the same grid.
constexpr uint32_t kDiagTaskPeriodMs = 1000;

// [DERIVED] Stack bytes, M5 (§16.1): DiagInput ≈ 216 B + the registry
// read-modify-write frame + the raw drain loops (≤ 192 sample copies) +
// FreeRTOS/libc frames; 3072 B keeps ~10x headroom over the M2 2048 B
// estimate. Re-measure uxTaskGetStackHighWaterMark at DoD (§16.5).
constexpr uint32_t kDiagTaskStackBytes = 3072;

// [DERIVED] Priority 2: ≤ 3 per the W2 convention and one rung below
// F03's pump (3), so diagnostics never delays the decision path.
constexpr uint32_t kDiagTaskPriority = 2;

// [runtime.md §3.2] Core 1 — the pipeline core; BLE/storage stay on 0.
constexpr uint32_t kDiagTaskCore = 1;

// [HW] uxTaskGetStackHighWaterMark unit: this platform returns BYTES, not
// vanilla-FreeRTOS words — the shipped IDF 4.4 header states verbatim
// "the minimum free stack space there has been (in bytes not words,
// unlike vanilla FreeRTOS)"
// (framework-arduinoespressif32/tools/sdk/esp32/include/freertos/include/
// freertos/task.h, doc comment above uxTaskGetStackHighWaterMark,
// verified 2026-10-18). The plan's [ASSUMED] x4 words is therefore
// CONTRADICTED by the header; the conversion factor is 1 and lives ONLY
// here (host-tested below).
constexpr uint32_t kStackWatermarkUnitBytes = 1;

// The one watermark-unit → bytes conversion (see kStackWatermarkUnitBytes
// for the verified source); saturates instead of truncating so a huge
// reading can never wrap into a small-looking number.
constexpr uint16_t stackWatermarkBytes(uint32_t watermark_units) {
    const uint32_t bytes = watermark_units * kStackWatermarkUnitBytes;
    return (bytes > 0xFFFFu) ? 0xFFFFu : static_cast<uint16_t>(bytes);
}

// --- output seam (plan D5) ----------------------------------------------------

// Copies the newest published SourceQuality; false before the first tick
// ("no value yet" — consumers must treat that as absent, not as Good).
bool latestSourceQuality(bw::SourceQuality& out);

// Same for the newest HealthSnapshot.
bool latestHealth(bw::HealthSnapshot& out);

// --- M5 seams (plan §16.1) ----------------------------------------------------

// D13: records one completed cycle of `slot` (single-writer-per-slot
// read-modify-write: get → TaskHealth::noteCycle → set; LatestValue's
// critical section makes the copy tear-free for readers). Invalid slot
// (TaskSlot::Count / out of range) is a defined no-op. P4 adopts each
// independently scheduled task; never-received slots stay
// valid=false = excluded from every max (unknown ≠ 0).
void noteTaskCycle(bw::diag::TaskSlot slot, bw::TimeMs now_ms, uint32_t lateness_ms,
                   uint16_t stack_free_bytes);

// D13 accessor: false before that slot's first note ("no data yet",
// 02 §1 validity rule — never a default-constructed fake report).
bool latestTaskHealth(bw::diag::TaskSlot slot, bw::TaskHealth& out);

// D10: binds F26's CR-030 registry read path. nullptr is honest
// (unwired — own-only fallback, D11). May be called before or after
// wire(); rebinding replaces the previous pointer (wire_ble
// attachProviders precedent). Pointer publication is atomic; the caller must
// keep any previously published provider alive until readers have stopped.
void attachReliabilityProvider(bw::IReliabilityStateProvider* provider);

// Single producer = SensorTask, after acquisition. DiagTask consumes a
// mailbox copy, never races F01's plain mutable driver-status fields.
void noteSensorStatuses(bw::SensorStatus imu, bw::SensorStatus ppg);

// D15: F28-RawTee-shaped producer tee — F01's SensorTask pushes each raw
// sample here (P4 one-liners in wire_hal.cpp, F01's Owns). SPSC: this is
// the producer side; gatherInput() (DiagTask) is the consumer. Err::Full
// is REFUSED and counted by SpscQueue::drops() (CR-004) — never silently
// ignored; the drop count drives the D15 drop-hole rule below.
void onImuSample(const bw::ImuSample& sample);
void onPpgSample(const bw::PpgSample& sample);

// One DiagInput per tick (§15.4/§16.1): drains the raw tee into static
// window arrays, snapshots the task registry mailboxes, copies the
// reliability/BLE values at this seam (D10/D12). Declared here so the
// suite's no-heap test can drive the drain loops from its own TU; the
// P4 device path is diagTask → this → runTick. Reads ctx read-only.
bw::diag::DiagInput gatherInput(const app::AppContext& ctx, bw::TimeMs now_ms);

}  // namespace f23
