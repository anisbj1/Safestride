#pragma once

// bw/state/state_pump.h — F07 §15: the 1 Hz pump (runtime §3.2) as PURE
// LOGIC, extracted from wire_state.cpp so the host suite can drive it with a
// FakeClock (auditor A-03; spec D15). The platform task shell stays in the
// wiring (vTaskDelayUntil, clock read, task creation); everything else —
// sequence-gated mailbox pulls, feature-quality staleness aging, the engine
// step, snapshot/record publishing and drop counting — lives here: no
// platform calls, no allocation (static storage in the wiring), time passed
// in as data (D4), bw_core contracts only.

#include <bw/context.h>
#include <bw/features.h>
#include <bw/mode_profile.h>
#include <bw/model.h>
#include <bw/providers.h>
#include <bw/state/state_engine.h>

namespace bw::state {

// Non-owning ports the pump reads/publishes. Bound by the wiring piece by
// piece (ctx-side fields at wire(), the W1 mailboxes via attachMailboxes,
// F17 via attachEventSink); a nullptr port is an honest never-received axis
// (the gates turn it into Unknown + SensorUnavailable, never a fabricated
// Awake). Owner: the composition root (01 §1 rule 3).
struct PumpPorts {
    const bw::LatestValue<bw::MotionFeatures>* motion = nullptr;
    const bw::LatestValue<bw::PpgFeatures>* ppg = nullptr;
    const bw::LatestValue<bw::ActivityEstimate>* activity = nullptr;
    const bw::LatestValue<bw::WearEstimate>* wear = nullptr;
    const bw::LatestValue<bw::AlertnessProbabilities>* alertness = nullptr;
    const bw::LatestValue<bw::TimeMs>* wake_check_passed = nullptr;
    bw::LatestValue<bw::StateSnapshot>* snapshot = nullptr;
    bw::IEventSink* events = nullptr;  // nullptr until F17: records are counted dropped

    // P4b (spec D16): optional F23 quality provider — a plain function
    // pointer (no heap, no std::function, no include of another feature's
    // header), bound once by the composition root through
    // f07::attachQualityProvider. Returns true after writing `out` (F23 has
    // published at least once — [ASSUMED] D16); false/never-received leaves
    // the channel to age via kFeatureQualityStaleMs. Absent (nullptr) ⇒ the
    // feature-mailbox derivation runs byte-for-byte as before (missing ⇒
    // Insufficient, A-03) — regression-critical. Owner: composition root
    // (01 §1 rule 3).
    bool (*quality_provider)(bw::SourceQuality& out) = nullptr;
    // W6 P4: snapshot metadata only; never changes state/alert gates.
    const bw::IModeProvider* mode_provider = nullptr;
};

class StatePump {
public:
    explicit StatePump(const StateEngineConfig& cfg = StateEngineConfig{});

    // Bind all ports before starting the task; mutation during tick is unsafe.
    PumpPorts& ports() { return ports_; }
    StateEngine& engine() { return engine_; }

    // One 1 Hz cycle: pull NEW mailbox values (sequence-gated), age the
    // feature-quality channel (kFeatureQualityStaleMs), substitute the bound
    // quality provider for the derivation (D16, P4b), step the engine with
    // time as data (D4) and publish the snapshot / transition records (D11).
    // Returns the engine outputs (tests assert on them or on the mailbox).
    StateOutputs tick(TimeMs now);

    // Pipeline health (H7): when the engine last stepped, transition records
    // dropped because no sink was attached or publish() was rejected (D11).
    TimeMs lastRunMs() const { return last_run_ms_; }
    uint32_t droppedEvents() const { return dropped_events_; }

private:
    StateEngine engine_;
    PumpPorts ports_;

    // Held inputs: the last value of each axis (file-static storage in the
    // wiring — no heap). The engine sees producer gaps as staleness because
    // the stamps age against in_.t_ms (never re-marked fresh — the sequence
    // gate is the only thing that refreshes them).
    StateInputs in_{};

    // Per-port sequence cursors (0 = never published; an empty channel is an
    // ordinary state, never a stall).
    uint32_t seen_motion_ = 0;
    uint32_t seen_ppg_ = 0;
    uint32_t seen_activity_ = 0;
    uint32_t seen_wear_ = 0;
    uint32_t seen_alertness_ = 0;
    uint32_t seen_wake_check_passed_ = 0;

    // Feature-quality channel per axis: missing => Insufficient, never a
    // default "Good" (00 §1); stamped at receipt and aged back to
    // Insufficient after kFeatureQualityStaleMs (auditor A-03).
    bw::DataQuality imu_quality_ = bw::DataQuality::Insufficient;
    bw::DataQuality ppg_quality_ = bw::DataQuality::Insufficient;
    TimeMs imu_quality_stamp_ms_ = 0;
    TimeMs ppg_quality_stamp_ms_ = 0;

    // P4b provider channel (D16): the channel that reaches the engine while
    // quality_provider is bound (full substitution — F23 IS the axis
    // producer, context §6). Each `true` receipt writes BOTH axes + the
    // receipt stamp; `false`/silence ages back to Insufficient through the
    // SAME kFeatureQualityStaleMs `aged()` machinery (no new constant).
    // Insufficient default: a never-received provider is honest, never a
    // default Good (00 §1). Measured cost: 16 B members (2 B SourceQuality +
    // 6 B alignment padding to the 8-byte-aligned TimeMs stamp + 8 B stamp;
    // host sizeof probe, xtensa aligns uint64_t to 8 identically) + the 4 B
    // port pointer on ESP32 — see §15 budget.
    bw::SourceQuality provider_quality_ = {bw::DataQuality::Insufficient,
                                           bw::DataQuality::Insufficient};
    TimeMs provider_quality_stamp_ms_ = 0;

    TimeMs last_run_ms_ = 0;
    uint32_t dropped_events_ = 0;
};

}  // namespace bw::state
