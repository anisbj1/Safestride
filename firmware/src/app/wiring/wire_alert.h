#pragma once

// firmware/src/app/wiring/wire_alert.h - F10's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_alert.* per F10 spec §2
// Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).

#include <cstdint>

#include <bw/alert/alert_engine.h>

namespace app {
struct AppContext;
}

namespace f10 {

// Constructs the feature's engine (default thresholds from F10 §8) - nothing
// else is consumed yet: AppContext carries no actuator and no event sink
// (F00 D9 keeps it minimal), so output injection happens through
// attachOutputs() below.
// Idempotence contract: call once, in the "decision" slot of the README's
// fixed call order (after F07-F09's wire()).
//
// Task table landed at P4 (2026-10-12, integrator): wire() also creates the
// 20 Hz AlertTask (kAlertTickPeriodMs, F10 §11) now that AppContext carries
// the StateSnapshot mailbox and the Command queue - the channels this
// function was waiting for - and attaches the bench LED/buzzer through
// f01's accessors (composition-level binding, wire_ui.cpp precedent).
void wire(app::AppContext& ctx, bw::ICommandSink* mode_commands = nullptr);

// Sole ctx.commands consumer: alert pulses + relay to F24's separate SPSC.
// Refusals are returned to the caller; no retry/block or second producer.
uint32_t drainCommands(app::AppContext& ctx, bw::alert::PolicyInput& in,
                       bw::ICommandSink* mode_commands);
uint32_t droppedModeCommands();

// AlertTask body (runtime.md §3.2, declared per wiring README): every
// kAlertTickPeriodMs it reads ctx.clock, pulls the freshest StateSnapshot,
// drains ctx.commands into ONE-TICK acknowledge/cancel pulses (F10 §6.1:
// the policy does not edge-detect, so the pump converts - one delivery =
// one tick), steps the engine and publishes AlertStatus to ctx.alert_status
// on every level change. Created by wire(); `arg` is the AppContext.
void alertTask(void* arg);

// --- Task constants (runtime.md §3.2; F10 spec §8/§11) ------------------------

// [EXPERIMENTAL] Stack bytes: PolicyInput copy (~80 B) + AlertOutput +
// command drain + FreeRTOS frames; 2 KB matches the other decision pumps.
// Re-measure with uxTaskGetStackHighWaterMark at Validate.
constexpr uint32_t kAlertTaskStackBytes = 2048;

// [DERIVED] Priority 3: W2 convention caps new core-1 pumps at <= 3 and
// F10 shares rung 3 with InputTask (the hard debounce timing) and the
// other decision pumps; the 20 Hz tick (kAlertTickPeriodMs, the pattern
// quantisation bound) rather than the priority gives the alert path its
// responsiveness. [ASSUMED] the F07 spec comment says "Alert/Input tasks
// run strictly higher" than the decision rung - revisit with runtime.md
// §3.2 tuning at Validate if alert latency is ever measured late.
constexpr uint32_t kAlertTaskPriority = 3;

// [runtime.md §3.2] Core 1 - same core as its F07 producer, so the
// snapshot hand-off stays cache-local.
constexpr uint32_t kAlertTaskCore = 1;

// Composition-root accessor: the engine is private to wire_alert.cpp; the
// integrator hands it to the pump that ticks it and reads AlertOutput
// (01 §1 rule 3). Consumers never include F10 headers - they read
// LatestValue<AlertStatus> and AlertRaised events through bw_core channels.
bw::alert::AlertEngine& engine();

// Late dependency injection for the three outputs F10 §3 promises:
// IRgbLed + IBuzzer (via PatternPlayer) and IEventSink (AlertRaised).
// The composition root calls this once it has those instances; until then the
// engine runs policy-only and reports Err::NotReady on a raise instead of
// dropping it silently. NULL detaches an output again (bench/tests).
// Contract: attachOutputs() before the first tick; only the pump calls
// step(); non-reentrant (single AlertTask - one step at a time, no nested
// evaluation from an actuator or sink callback).
void attachOutputs(bw::IRgbLed* led, bw::IBuzzer* buzzer, bw::IEventSink* events);

// Level-change events the pump could not hand to a sink (null until F17, or
// a rejected publish) - counted, never silently lost (H7, F07 D11
// precedent). Err::NotReady with no sink attached is the expected state at
// M2 and is counted here rather than reported as a fault: no FaultCode
// fits "event sink missing" yet (append-only enum; new codes belong to
// F26/W6, backlog 16).
uint32_t droppedEvents();

}  // namespace f10
