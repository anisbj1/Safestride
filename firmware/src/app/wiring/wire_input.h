#pragma once

// firmware/src/app/wiring/wire_input.h - F11's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_input.* per F11 spec §2
// Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).

#include <cstdint>

#include <bw/alerts.h>
#include <bw/commands.h>
#include <bw/containers/latest_value.h>
#include <bw/input/input_engine.h>
#include <bw/providers.h>

namespace app {
struct AppContext;
}

namespace f11 {

// Legacy standard-policy helper (host-pinned compatibility). The device demo
// does NOT call this: F10 alone publishes accepted wake-check responses, so
// queue refusal/deadline races cannot create a false pass in F06/F07.
inline bool recordWakeCheckOnWarning(const bw::AlertStatus& alert, const bw::Command& command,
                                     bw::LatestValue<bw::TimeMs>& passed_at) {
    if (alert.level != bw::AlertLevel::Warning ||
        command.type != bw::CommandType::AcknowledgeAlert) {
        return false;
    }
    passed_at.set(command.t_ms);
    return true;
}

inline bool pushWarningAck(const bw::AlertStatus* alert, const bw::Command& command,
                           bw::LatestValue<bw::TimeMs>& passed_at, bw::ICommandSink& sink) {
    if (alert != nullptr) {
        (void)recordWakeCheckOnWarning(*alert, command, passed_at);
    }
    return sink.push(command);
}

// Constructs the F11 graph: the static InputEngine (every timing value is a
// named constant from bw/input/input_constants.h, F11 §8) and - buttons exist
// only on the device - creates InputTask, which samples F01's IButtonInput at
// 100 Hz, derives the InputContext from ctx.alert_status (F11 D4), steps the
// engine once per tick and pushes each routed command into ctx.commands through
// a QueueCommandSink (F11 D6/D8).
// Idempotence contract: call once, in the "services" slot of the README's
// fixed call order (after f01::wire and f10::wire).
void wire(app::AppContext& ctx);

// InputTask body (runtime.md §3.2: F11 owns InputTask), declared here per the
// wiring README; `arg` is the AppContext. Defined only under ARDUINO - pure
// library logic stays host-testable, only this glue is device-side.
void inputTask(void* arg);

// --- InputTask constants (runtime.md §3.2: F11 owns InputTask) -------------

// [DERIVED] 100 Hz: two samples per kDebounceMs window, so the 20 ms debounce
// resolves within ±10 ms - one task period (F11 §11 budget). Also the cadence
// every boundary test in the suite assumes.
constexpr uint32_t kInputTaskPeriodMs = 10;

// [EXPERIMENTAL] The task moves two raw GPIO reads + one O(1) engine step per
// tick; frames are a few hundred bytes (engine StepOutput ~100 B on the stack).
// 2.5 KB leaves headroom over ProcessingTask's 2 KB for the deeper chain
// engine->router->detector; re-measure with uxTaskGetStackHighWaterMark at DoD
// once the task actually runs (F11 §11).
constexpr uint32_t kInputTaskStackBytes = 2560;

// [DERIVED] 3 (med): strictly below SensorTask (5, wire_hal.h) and
// ProcessingTask (4, wire_f02.h) - the sensor path has hard FIFO/queue budgets
// it can miss, button input has none to lose - and equal to the F03 pump (3),
// both being microsecond-scale periodic tasks where round-robin fairness holds.
// Responsiveness AND stuck detection both survive that rank: per tick the work
// is µs-scale, and every rule (debounce window, long-press, the 10 s stuck
// threshold) is evaluated from timestamps, not from sample counts - so a late
// tick shifts latency (bounded by the 20 ms / 800 ms windows), never
// correctness: a button held across a starvation gap crosses kButtonStuckMs
// and is reported at the first tick that gets to run (F11 D3/D5).
constexpr uint32_t kInputTaskPriority = 3;

// [runtime.md §3.2] Core 1 - same core as SensorTask/ProcessingTask; the raw
// levels and ctx mailboxes are single-word reads/writes anyway, but keeping
// one core avoids cross-core cache-line traffic for a 100 Hz task.
constexpr uint32_t kInputTaskCore = 1;

}  // namespace f11
