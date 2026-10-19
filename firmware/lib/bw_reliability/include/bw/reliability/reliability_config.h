#pragma once

// bw/reliability/reliability_config.h — F26's named constants (spec §16.3).
// Values tagged [EXPERIMENTAL] are starting points: tune with recorded data
// (F28/F29), they are not facts about the hardware.

#include <cstddef>
#include <cstdint>

namespace bw::reliability {

// --- watchdog supervision (spec §16.3) --------------------------------------

// [EXPERIMENTAL] Why 5 s: §8 as accepted — the TWDT limit is longer than 5x
// the slowest supervised period (1 s SupervisorTask grid), so ordinary jitter
// never trips it; a genuinely dead Tier A task is reset within ~15 s of its
// death (grace + stall + TWDT). Tune with recorded task lateness (F28/F29).
constexpr uint32_t kTaskWatchdogTimeoutS = 5;

// [DERIVED] Why 3: three missed consecutive heartbeats distinguish jitter
// (1x period, absorbed) from death without false resets — a task late by more
// than 3 of its own periods has missed every beat inside one full cycle
// (spec §16.3).
constexpr uint32_t kStallFactor = 3;

// [DERIVED] Why 10 000 ms: boot grace — tasks are created staggered by the
// wire ladder, and 10 s is 10x the longest (1 s) period, so slowest-starting
// tasks are never blamed for the ladder itself (spec §16.3).
constexpr uint32_t kSupervisionGraceMs = 10000;

// [DERIVED] W6 P4: 11 original tasks plus wear/wake/sleep/cataplexy,
// event-log, baseline and optional research. Supervisor owns the feed and
// cannot supervise itself; F23 tracks its health separately.
constexpr size_t kMaxSupervisedTasks = 18;

// --- fault registry (spec §16.3, D3/D4) -------------------------------------

// [DERIVED] Why 29: FaultCode values 0–28 exist today (bw/fault.h: None…
// EventPublishDrop after CR-031 BaselineCorrupt=25, CR-032 LowStack=26 and
// CR-033 TaskCreateFailed=27 / EventPublishDrop=28; next free value is 29).
// Codes >= this value are a defined no-op in FaultRegistry (F23 I2
// precedent), never UB. The static_assert guards CR-030's single 32-bit
// fault_bits word: past 32 codes the registry needs a second word, which is
// a change request.
constexpr size_t kMaxFaultCodes = 29;
static_assert(kMaxFaultCodes <= 32, "CR-030 fault_bits is one 32-bit word; more codes need a CR");

// [DERIVED] Why 26: detector_id convention = the feature number (F07=7,
// F14=14 in their own configs); 26 = F26, pinned by spec D13/§16.8 test 7.
constexpr uint8_t kFaultDetectorId = 26;

// --- boot-loop guard / crash record (spec §16.3, D10/D11) -------------------

// [EXPERIMENTAL] Why 3: §8 as accepted — safe mode on the 3rd consecutive
// boot that dies before kBootLoopBootLimitMs (D10's streak rule: 2 fast boots
// stay normal, the 3rd is safe mode). Tune with recorded reset behaviour.
constexpr uint8_t kMaxRebootsInWindow = 3;

// [EXPERIMENTAL] Why 600 000 ms: replaces wall-clock "10 min" (no RTC, wall
// time may be 0 — runtime.md §4): uptime a boot must survive to clear the
// reboot streak and exit safe mode on the NEXT boot (D10).
constexpr uint32_t kBootLoopBootLimitMs = 600000;

// [DERIVED] Why "f26.crash1": one KV record per D11; the schema suffix keeps a
// future format migration from reading old bytes as new ones (spec §16.3).
constexpr const char* kCrashRecordKey = "f26.crash1";

// [DERIVED] Why 1: core.md §1 versioning rule for persisted data — the first
// on-flash format, bumped by any layout change.
constexpr uint8_t kCrashRecordSchema = 1;

// [EXPERIMENTAL] Why 30 s: retry a transient KV failure without issuing a
// flash write every 1 s supervisor tick; tune from storage recovery traces.
constexpr uint32_t kCrashWriteRetryMs = 30000;

// --- sensor reinit scheduling (spec §16.3, D12) -----------------------------

// [EXPERIMENTAL] Why 1 000 ms: §8 as accepted — first retry one second after a
// failed init, so a transient bus fault clears on its own before we poke again.
constexpr uint32_t kReinitBackoffInitialMs = 1000;

// [EXPERIMENTAL] Why 30 000 ms: §8 cap for the x2 backoff — a dead bus is
// retried at most every 30 s (never hammered, spec §8/D12).
constexpr uint32_t kReinitBackoffMaxMs = 30000;

// --- degradation policy inputs (spec §16.3, rows 3/4) -----------------------

// [EXPERIMENTAL] Why 10 %: below roughly one free segment the F18 store starts
// refusing/evicting (row 3); exact geometry is F18's — tune when the segment
// layout is final.
constexpr uint32_t kStorageDegradedFreePct = 10;

// [EXPERIMENTAL] Why 30 000 ms: an absent phone is the normal state — only a
// link that WAS established and then stayed lost for 30 s confirms BleDown
// (row 4), so a user walking out of range does not flap the flag.
constexpr uint32_t kBleDownConfirmMs = 30000;

// [EXPERIMENTAL] Why 60 000 ms: fault.h K / F23 kFaultRepeatMs convention —
// flapping facilities emit at most one SensorFault event per minute (D13(i)).
constexpr uint32_t kSensorFaultRepeatMs = 60000;

}  // namespace bw::reliability
