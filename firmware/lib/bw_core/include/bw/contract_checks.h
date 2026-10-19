#pragma once

// bw/contract_checks.h — compile-time contract enforcement (F00 spec §6 D3, layer a).
//
// Purpose: any field/type/layout drift in a contract type fails the BUILD, not a
// downstream feature at integration. 02 §6.6 requires a CR for exactly such a change,
// so this friction is deliberate. Zero flash cost: all asserts.
//
// What is pinned here: sizeof + alignof + std::is_trivially_copyable_v for every
// contract VALUE type, plus Event's field offsets (CR-002's load-bearing order).
// Deliberately NOT here: enum numeric values — those live in the Unity tests
// (test_contract_layout.cpp, test_fault_codes.cpp) where a failure prints a message
// naming the enum (D3 layer b).
//
// Types with pointers are excluded from size/align pins: sizeof(SettingSpec) differs
// between 64-bit host and 32-bit Xtensa by design (const char* name) — only its
// trivially-copyable property is portable and asserted.

#include <cstddef>
#include <type_traits>

#include <bw/alerts.h>
#include <bw/commands.h>
#include <bw/context.h>
#include <bw/events.h>
#include <bw/fault.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/input.h>
#include <bw/model.h>
#include <bw/providers.h>
#include <bw/quality.h>
#include <bw/result.h>
#include <bw/state.h>
#include <bw/task_health.h>
#include <bw/types.h>

// -- helper macros (undef'd at the end) ---------------------------------------
// BW_CONTRACT(type, bytes, align): trivially copyable + exact size + exact alignment.
#define BW_CONTRACT(Type, Bytes, Align)                                                            \
    static_assert(std::is_trivially_copyable_v<Type>, #Type " must be trivially copyable "         \
                                                            "(02 §1)");                            \
    static_assert(sizeof(Type) == (Bytes), #Type " layout drifted: expected " #Bytes " B");        \
    static_assert(alignof(Type) == (Align), #Type " alignment drifted: expected " #Align)

// BW_VALUE(type): trivially copyable only (size varies with pointer width).
#define BW_VALUE(Type)                                                                             \
    static_assert(std::is_trivially_copyable_v<Type>, #Type " must be trivially copyable (02 §1)")

namespace bw {
namespace contract {

// -- 02 §3 hardware -----------------------------------------------------------
BW_CONTRACT(ImuSample, 32, 8);
BW_CONTRACT(PpgSample, 24, 8);
BW_CONTRACT(Rgb, 3, 1);
BW_CONTRACT(BatteryReading, 16, 4);

// -- 02 §4.1 quality ----------------------------------------------------------
BW_CONTRACT(SourceQuality, 2, 1);

// -- 02 §4.2/4.3 processing outputs + context ---------------------------------
BW_CONTRACT(MotionFeatures, 40, 8);
BW_CONTRACT(PpgFeatures, 40, 8);
BW_CONTRACT(ActivityEstimate, 16, 8);
BW_CONTRACT(WearEstimate, 16, 8);

// -- 02 §4.4 ML layer ---------------------------------------------------------
BW_CONTRACT(FeatureVector, 144, 8);
BW_CONTRACT(ModelOutput, 40, 4);
BW_CONTRACT(Contribution, 8, 4);
BW_CONTRACT(WakeMotionEvidence, 16, 8);      // CR-034: timestamp + validity/qualification
BW_CONTRACT(AlertnessProbabilities, 56, 8);  // CR-034: appended evidence

// -- 02 §4.5 state + events ---------------------------------------------------
BW_CONTRACT(StateSnapshot, 48, 8);   // CR-034: appended evidence
BW_CONTRACT(Event, kEventBytes, 8);  // 48 B per CR-002 (F00 §8/§11)
BW_CONTRACT(UserLabel, 1, 1);
BW_CONTRACT(EventType, 1, 1);

// -- 02 §4.6 alerts / input / commands ----------------------------------------
BW_CONTRACT(AlertStatus, 24, 8);
BW_CONTRACT(ButtonEvent, 16, 8);
BW_CONTRACT(Command, 32, 8);

// -- 02 §5 providers ----------------------------------------------------------
BW_CONTRACT(BaselineProfile, 20, 4);
BW_CONTRACT(HealthSnapshot, 32, 8);
BW_CONTRACT(SettingValue, 8, 4);
BW_VALUE(SettingSpec);  // const char* name: width differs host vs target

// -- 02 §7 F00-defined types --------------------------------------------------
BW_CONTRACT(TaskHealth, 24, 8);

// -- primitives (02 §2) -------------------------------------------------------
BW_VALUE(Result<int>);
BW_VALUE(Result<void>);

// -- Event field offsets (CR-002's load-bearing order; see events.h) ----------
static_assert(offsetof(Event, t_start_ms) == 0, "Event layout: t_start_ms must be first (CR-002)");
static_assert(offsetof(Event, seq) == 8, "Event layout: seq drifted");
static_assert(offsetof(Event, boot_id) == 12, "Event layout: boot_id drifted");
static_assert(offsetof(Event, duration_ms) == 16, "Event layout: duration_ms drifted");
static_assert(offsetof(Event, t_start_unix) == 20, "Event layout: t_start_unix drifted");
static_assert(offsetof(Event, confidence) == 24, "Event layout: confidence drifted");
static_assert(offsetof(Event, reasons) == 28, "Event layout: reasons drifted");
static_assert(offsetof(Event, payload) == 32,
              "Event layout: payload must be 8-byte aligned (CR-002)");
static_assert(offsetof(Event, quality) == 40, "Event layout: quality drifted");
static_assert(offsetof(Event, type) == 42, "Event layout: type drifted");
static_assert(offsetof(Event, label) == 43, "Event layout: label drifted");
static_assert(offsetof(Event, context) == 44, "Event layout: context drifted");
static_assert(offsetof(Event, alertness_at_start) == 45, "Event layout: alertness drifted");
static_assert(offsetof(Event, detector_id) == 46, "Event layout: detector_id drifted");
static_assert(offsetof(Event, schema_version) == 47,
              "Event layout: schema_version must be last (CR-002)");

// Padding-free proof for the record that DEC-012 persists: the last member ends
// exactly at sizeof — no trailing padding.
static_assert(offsetof(Event, schema_version) + sizeof(uint8_t) == sizeof(Event),
              "Event must end exactly on schema_version: zero trailing padding (CR-002)");

// -- version pins -------------------------------------------------------------
static_assert(Event::kSchemaVersion == 1, "Event schema version bumped without a CR");
static_assert(kEventBytes == 48, "kEventBytes must stay 48 (CR-002, F00 §11)");

}  // namespace contract
}  // namespace bw

#undef BW_CONTRACT
#undef BW_VALUE
