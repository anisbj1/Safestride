#pragma once

// bw/sleep/sleep_episode_tracker.h — F08 §6 D1: the Candidate → Active episode
// machine with gap merging (no pause state — NotWorn/Unknown are ordinary gap
// time). Pure logic in `Output step(const Input&)` style (00 §7.2): one
// StateSnapshot in, bookkeeping + at most one Event value out. Events leave the
// module as values and the wiring adapter publishes them via IEventSink (§3),
// so the module has no I/O on the event path and is directly scriptable in
// host tests (§7). Compile-time dependencies: bw_core only (01 §1 rule 1).

#include <bw/clock.h>
#include <bw/events.h>
#include <bw/providers.h>
#include <bw/sleep/sleep_config.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::sleep {

// §6 D4 open-marker record: a liveness flag, NOT episode data — written once
// when an episode is promoted to Active, cleared at close/recover. Trivially
// copyable and schema-versioned: a mismatch (or a corrupt/short blob) means
// "ignore the marker" instead of decoding garbage (§8).
struct OpenMarkerRecord {
    uint8_t schema_version = 0;       // kMarkerSchemaVersion; 0 = "cleared"
    uint8_t reserved[3] = {0, 0, 0};  // §6 D7 habit: unused bytes written as 0
    UnixSeconds t_start_unix = 0;     // wall time at candidate start; 0 if never synced
};

// One step's result (§7). `status_changed` guards the LatestValue write in the
// wiring: setting the mailbox every tick would inflate its overwrites()
// counter, which is a real data-loss counter (F00 D10).
struct StepOutput {
    bool has_event = false;
    Event event{};
    bool status_changed = false;
    EpisodeStatus status{};
};

class SleepEpisodeTracker {
public:
    // Both optional dependencies degrade gracefully (§6 D4, §7):
    //  kv         — nullptr: no persistence, no recovery (the wave-2 build, §13);
    //               a missing marker is the normal case, never a fault (§10).
    //               Non-const on purpose: 02 §5's get/put are non-const.
    //  wall_clock — nullptr: wall time unknown; classification falls to the
    //               honest rows and never claims day/night (DEC-009, §6 D2).
    explicit SleepEpisodeTracker(const SleepEpisodeConfig& cfg, IKeyValueStore* kv = nullptr,
                                 const IClock* wall_clock = nullptr);

    // One snapshot in (§6 D3): timing comes only from snapshot t_ms — never
    // from wall time — and an identical t_ms is a no-op (row 1), because the
    // wiring polls IStateProvider::latest() faster than snapshots advance.
    StepOutput step(const StateSnapshot& snapshot);

    // Called once per boot, before the first step() (§6 D4): reads the open
    // marker and, if one survived a reboot, returns exactly one recovered
    // event (class Uncertain, kFlagRecovered, duration 0, previous-boot wall
    // time) and clears the marker. Second call: nothing.
    StepOutput recover();

private:
    // Transitions (§6 D1 rows 3/6/7/8). They mutate phase/bookkeeping and fill
    // `out`; step() owns the single status write per path.
    void promote(StepOutput& out, const StateSnapshot& s, TimeMs span);
    void closeActive(StepOutput& out, TimeMs t_close_ms, const StateSnapshot& s,
                     bool force_uncertain, ReasonMask extra_reasons);
    void resetEpisode();

    EpisodeStatus currentStatus(TimeMs t_ms) const;
    float degradedShare() const;                          // §6 D5, 0 when no observed time
    float runningConfidence(float degraded_share) const;  // mean × (1 − share), clamped
    void fillEventShell(Event& e, const StateSnapshot& s) const;
    void writeMarker();  // §6 D4: once per Active episode
    void clearMarker();  // schema-0 record: "cleared" (no remove in 02 §5)

    SleepEpisodeConfig cfg_;
    IKeyValueStore* kv_;  // non-const: 02 §5's get/put are non-const
    const IClock* wall_clock_;

    EpisodePhase phase_ = EpisodePhase::Idle;
    bool have_last_t_ = false;
    TimeMs last_t_ms_ = 0;   // t_ms of the previous snapshot (rows 1/8)
    TimeMs t_start_ms_ = 0;  // episode start (row 2); 0 when Idle
    // §6 D1 gap: OBSERVED non-PossibleSleep time since the last sleep-like
    // snapshot. A stale (silent) stretch has no snapshots — it is degraded
    // time (§6 D5), never a gap; NotWorn/Unknown/Insufficient snapshots are
    // ordinary observed gap time.
    TimeMs gap_ms_ = 0;
    UnixSeconds t_start_unix_ = 0;             // wall time sampled ONCE at candidate start
    OperatingMode mode_ = OperatingMode::Day;  // mode of the episode's evidence
    ActivityContext activity_ = ActivityContext::Unknown;  // context at candidate start
    // §6 D5 time-weighted accumulators: per step, Δt joins total_ms_, and
    // sleep_like_ms_ only when the step was sleep-like with both sources usable.
    TimeMs sleep_like_ms_ = 0;
    TimeMs total_ms_ = 0;
    float confidence_sum_ = 0.0f;  // mean of per-snapshot confidences (D5)
    uint32_t snapshot_count_ = 0;
    bool recovered_ = false;  // recover() runs at most once per boot
};

}  // namespace bw::sleep
