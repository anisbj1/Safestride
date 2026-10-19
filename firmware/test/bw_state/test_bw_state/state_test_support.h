// F07 test support — helpers shared by the bw_state suite.
//
// freshInputs(t) builds one (t_ms, inputs) row describing a fully fresh, healthy
// 1 Hz pipeline in calm state (evidence_drowsy 0.3 < the 0.40 calm line), so
// each test only states what it varies. Time is data (D4): no clock object,

#pragma once

#include <bw/state/state_engine.h>
#include <bw/state/state_types.h>

namespace f07_test {

inline bw::state::StateInputs freshInputs(bw::TimeMs t_ms) {
    bw::state::StateInputs in{};
    in.t_ms = t_ms;
    in.alertness.t_ms = t_ms;
    in.alertness.valid = true;
    in.alertness.awake = 0.7f;
    in.alertness.drowsy = 0.2f;
    in.alertness.sleep = 0.1f;  // drowsy + sleep = 0.3: calm, below every enter
    in.alertness.confidence = 0.8f;
    in.alertness.input_quality = bw::DataQuality::Good;
    in.alertness.reasons = 0;
    in.wear.t_ms = t_ms;
    in.wear.state = bw::WearState::Worn;
    in.wear.confidence = 0.9f;
    in.activity.t_ms = t_ms;
    in.activity.context = bw::ActivityContext::Sitting;
    in.activity.confidence = 0.7f;
    in.quality.imu = bw::DataQuality::Good;
    in.quality.ppg = bw::DataQuality::Good;
    in.mode = bw::OperatingMode::Day;
    in.power = bw::PowerProfile::Normal;
    return in;
}

inline bw::ReasonMask bitOf(bw::ReasonCode c) {
    return static_cast<bw::ReasonMask>(1u << static_cast<uint8_t>(c));
}

}  // namespace f07_test
