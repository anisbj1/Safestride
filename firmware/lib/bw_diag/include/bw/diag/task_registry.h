#pragma once

// bw/diag/task_registry.h — F23 §16.2 D13/D14: the feature-local TaskSlot
// keys and the pure helpers over a bw::TaskHealth snapshot array.
//
// D13: the registry itself (kMaxTrackedTasks mailboxes) lives at
// the wiring seam (wire_diag.cpp); these helpers are the pure-logic view
// the composer consumes (no platform calls, host-testable — 01 §1 rule 5).
//
// Honesty rules (integrator ruling D13: "invalid slots excluded;
// unknown ≠ 0"):
//  - a slot with valid == false never contributes evidence: it is not
//    counted as 0 ms late, and it never reads as 0 free stack bytes;
//  - TaskHealth.stack_free_min_bytes == 0 means "not yet measured"
//    (bw/task_health.h) — unknown, so it never triggers LowStack either;
//  - the empty array / nullptr first-call state is the same "no evidence"
//    state: 0 / 0 / false.

#include <cstdint>

#include <bw/diag/diag_config.h>
#include <bw/task_health.h>

namespace bw::diag {

// Registry keys (§16.2 D13): original table keys retained; W6 P4 appends
// independently scheduled pumps. A slot value is an array index and must
// never be renumbered. Never-received slots stay valid=false = excluded.
enum class TaskSlot : uint8_t {
    Sensor = 0,    // F01 SensorTask
    Processing,    // F02 ProcessingTask
    Intelligence,  // F03 activity pump ONLY (legacy key retained)
    State,         // F07 state pump ONLY
    Alert,         // F10 AlertTask
    Input,         // F11 InputTask
    Ui,            // F12 UiTask
    Storage,       // F18 StorageTask
    Comms,         // F19 CommsTask
    ServicePump,   // integrator ServicePump (W4 P4)
    Supervisor,    // F26 SupervisorTask
    DiagSelf,      // F23 DiagTask (kDiagSelfTaskSlot)
    Wear,          // F04 pump (independent task, never grouped with F03)
    Wake,          // F06 pump
    Sleep,         // F08 pump
    Cataplexy,     // F09 pump
    EventLog,      // F17 pump
    Baseline,      // F13 pump
    Research,      // F28 optional capture task
    Count,         // sentinel == kMaxTrackedTasks; never a real slot
};

static_assert(static_cast<uint8_t>(TaskSlot::Count) == kMaxTrackedTasks,
              "one TaskSlot per kMaxTrackedTasks registry entry (§8)");
static_assert(static_cast<uint8_t>(TaskSlot::DiagSelf) == kDiagSelfTaskSlot,
              "DiagSelf must be the kDiagSelfTaskSlot table entry (§8)");

// Worst lateness over VALID slots (keeps the max — TaskHealth::noteCycle's
// own rule, F00 §9). 0 = no valid slot: "no evidence", which is never
// > kMaxTaskLatenessMs, so it diagnoses nothing (unknown ≠ late).
uint16_t worstLatenessMs(const bw::TaskHealth* tasks, uint8_t count);

// Smallest measured watermark over VALID slots (a slot that has not
// measured, stack_free_min_bytes == 0, is skipped: unknown ≠ 0-free).
// 0 = no measured evidence — never a trigger on its own; fault decisions
// must go through slotBelowStackFloor(), which encodes the floor rule.
uint16_t worstStackFreeBytes(const bw::TaskHealth* tasks, uint8_t count);

// D14 LowStack rule: true iff at least one VALID slot carries a measured
// watermark strictly below floor_bytes (fault.h: <
// kMinTaskStackFreeBytes). No valid slot ⇒ false — an unmonitored task is
// unknown, never "stack exhausted".
bool slotBelowStackFloor(const bw::TaskHealth* tasks, uint8_t count, uint16_t floor_bytes);

}  // namespace bw::diag
