#pragma once

// bw/diag/diag_types.h — F23 §15.1 feature-local value types (NOT contracts):
// SystemStats, FaultEdge, DiagInput, DiagOutput.
//
// The plan (§15.1) lists these under bw/diag/diag_types.h's eventual parent
// bw/diag/diagnostics.h; they live here instead so the assessors can use
// SystemStats/FaultEdge without including diagnostics.h (which includes the
// assessors — a cycle). diagnostics.h includes this header, so plan §15.1's
// public surface is unchanged.
//
// SystemStats satisfies the §3 `ISystemStats` provider as a plain value
// struct filled by wiring (D4): no bw_core contract was added, and on
// native tests construct it directly.

#include <cstddef>
#include <cstdint>

#include <bw/diag/diag_config.h>
#include <bw/fault.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/providers.h>
#include <bw/quality.h>
#include <bw/task_health.h>
#include <bw/types.h>

namespace bw::diag {

// Wiring-gathered system counters (D4). valid=false ⇒ heap/lateness/BLE
// unknown: fields are reported as 0 and NO fault is raised (unknown ≠ fault).
struct SystemStats {
    bool valid = false;
    uint32_t min_free_heap = 0;           // ESP.getMinFreeHeap() (device)
    uint16_t worst_task_lateness_ms = 0;  // max over instrumented tasks
    bool ble_connected = false;           // false until F19 (W4), honest
};

// One diagnosed-condition transition (spec §15.1). code == FaultCode::None
// means "no edge this tick" (fault.h's own None convention — never invent
// a 0-means-what).
struct FaultEdge {
    FaultCode code = FaultCode::None;
    FaultSeverity severity = FaultSeverity::Warning;
};

// Everything F23 judges from, gathered once per 1 Hz tick by wiring (§15.4).
struct DiagInput {
    TimeMs now_ms = 0;
    bool has_imu_status = false;
    SensorStatus imu_status = SensorStatus::Uninitialized;
    bool has_ppg_status = false;
    SensorStatus ppg_status = SensorStatus::Uninitialized;
    bool has_motion = false;
    MotionFeatures motion{};  // age = now_ms - t_ms, checked inside (§15.1)
    bool has_ppg = false;
    PpgFeatures ppg{};
    BatteryReading battery{};  // valid=false (bench, no divider) ⇒ unknown
    SystemStats stats{};       // valid=false ⇒ heap/lateness unknown
    // Raw windows: nullptr at M2 on device (no legal raw tap — D6); the raw
    // checks then report "not evaluated", never a guess. Host tests feed
    // fixtures here. Device feed: M5 D15 — wire_diag drains the F23 tee
    // queues and applies the drop-hole rule (a drop ⇒ suppressed window).
    const ImuSample* imu_raw = nullptr;
    size_t imu_raw_n = 0;
    const PpgSample* ppg_raw = nullptr;
    size_t ppg_raw_n = 0;
    // M5 additions (§16.1):
    // D10 — CR-030 ReliabilityState copied ONCE at the wiring seam (value
    // copy, not a provider pointer: Diagnostics stays plain value logic).
    // has_reliability=false ⇒ F26's registry has no snapshot yet — fault_bits
    // falls back to F23's own sticky bits (D11; own-only is honest, not a
    // placeholder — never a fabricated zero or foreign bit).
    bool has_reliability = false;
    ReliabilityState reliability{};
    // D13 — pointer+count into the wiring tick snapshot (no struct copy,
    // same pattern as the raw windows). nullptr/0 = no registry yet ⇒
    // lateness/stack evidence is absent, excluded from every max (unknown ≠ 0).
    const bw::TaskHealth* tasks = nullptr;
    uint8_t task_count = 0;
};

// Per-tick outputs: committed quality (hysteresis applied), the Health-
// Snapshot fields, and the rate-limited report edges (D9).
struct DiagOutput {
    SourceQuality quality{DataQuality::Insufficient, DataQuality::Insufficient};
    HealthSnapshot health{};
    FaultEdge edges[kMaxFaultEdgesPerTick]{};
    uint8_t edge_count = 0;
};

}  // namespace bw::diag
