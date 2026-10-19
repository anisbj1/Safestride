#pragma once

// bw/events.h — docs/02_INTERFACES.md §4.5 (the persisted/transmitted Event record).
//
// Field order follows CR-002 (accepted 2026-09-30), NOT the original print order in
// 02 §4.5: the same fields, same types, same names — reordered so the struct packs to
// exactly kEventBytes with zero padding. 02's diff has not been applied yet (the
// integrator's step, §15 of F00's spec); F00 codes against the accepted CR.
//
// Consequences of touching this file: 02 §6.6 requires a CR for any field change;
// contract_checks.h fails the build on layout drift; F17 (assigns seq), F18
// (persists), F19/F20 (transmit), F28 (Python decoder) all consume this layout.

#include <cstdint>

#include <bw/context.h>
#include <bw/quality.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw {

enum class EventType : uint8_t {
    SleepEpisode,
    DrowsinessEvent,
    SuspectedCataplexy,
    UserMarked,
    AlertRaised,
    SensorFault,
    DeviceNotWorn,
    BatteryWarning,
    NapPlanned,
    NapCompleted,
    TimeSync,
    Boot,
    // … append only, never renumber (02 §6.6)
};

enum class UserLabel : uint8_t { Unlabeled, Confirmed, Rejected };

// [DERIVED] Pinned sizeof(Event) target (F00 §8): the sum of 02 §4.5's member sizes
// is exactly 48 B; the original field order wasted 8 B on padding (56 B), which is
// 16.7 % on every stored and synced event forever (D7/CR-002). Asserted by
// contract_checks.h and test_contract_layout.cpp.
constexpr size_t kEventBytes = 48;

// Fixed-size record, persisted to flash (DEC-007) and transmitted over BLE (02 §6.2/6.3).
struct Event {
    static constexpr uint8_t kSchemaVersion = 1;  // never released in another layout (CR-002)

    // [EXPERIMENTAL] Type-specific small payload (F00 §8). Why 8: keeps Event
    // fixed-size; anything bigger belongs in an event-window blob (F18).
    static constexpr uint8_t kMaxPayloadBytes = 8;

    // Field order is load-bearing (CR-002): TimeMs first (8-byte aligned), all
    // 1-byte fields last, so the struct packs to exactly kEventBytes with zero
    // padding and no packing pragma. Offsets are pinned in contract_checks.h.
    TimeMs t_start_ms;         // offset 0   monotonic within boot_id
    uint32_t seq;              // offset 8   assigned by F17, monotonic per device, never reused
    uint32_t boot_id;          // offset 12  increments each boot
    uint32_t duration_ms;      // offset 16
    UnixSeconds t_start_unix;  // offset 20  0 if unknown at creation time
    float confidence;    // offset 24  0..1, model/heuristic confidence, NOT clinical certainty
    ReasonMask reasons;  // offset 28
    uint8_t payload[kMaxPayloadBytes];  // offset 32 type-specific small payload (8-byte aligned)
    SourceQuality quality;              // offset 40
    EventType type;                     // offset 42
    UserLabel label;                    // offset 43
    ActivityContext context;            // offset 44
    AlertnessState alertness_at_start;  // offset 45
    uint8_t detector_id;                // offset 46
    uint8_t schema_version;             // offset 47  written as kSchemaVersion
};

}  // namespace bw
