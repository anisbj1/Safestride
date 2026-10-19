#pragma once

// bw/state/state_types.h — F07 §7: StateInputs/StateOutputs, the engine's
// boundary. Trivially copyable value types (02 §1) so they ride queues and
// live on a task stack. Time is DATA (D4): the pump reads the clock, the
// engine never does — step() is a pure function of its argument (01 §1
// rule 5).

#include <type_traits>

#include <bw/context.h>
#include <bw/events.h>
#include <bw/model.h>
#include <bw/quality.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::state {

struct StateInputs {
    TimeMs t_ms;                       // monotonic "now", read by the pump (D4)
    AlertnessProbabilities alertness;  // bw/model.h (F06); reasons merged per D14
    WearEstimate wear;                 // bw/context.h (F04)
    ActivityEstimate activity;         // bw/context.h (F03)
    SourceQuality quality;             // bw/quality.h (F02 today, F23 later)
    OperatingMode mode;                // F24 via the pump; defaults until then (D10)
    PowerProfile power;                // F24 via the pump; defaults until then (D10)
    bool wake_check_passed = false;    // one-tick F10-accepted response; gates still outrank it
    bool short_motion_burst = false;   // one-tick F06 stillness-onset marker
};

struct StateOutputs {
    StateSnapshot snapshot;   // published to LatestValue<StateSnapshot> (1 Hz)
    bool transitioned;        // alertness axis changed at this step
    AlertnessState previous;  // valid when transitioned
    bool event_valid;         // a record must be published (D11)
    Event event;              // fully built; publish as-is (D11/D12)
};

static_assert(std::is_trivially_copyable_v<StateInputs>,
              "StateInputs rides queues/stack: trivially copyable (02 §1)");
static_assert(std::is_trivially_copyable_v<StateOutputs>,
              "StateOutputs rides the pump stack: trivially copyable (02 §1)");

}  // namespace bw::state
