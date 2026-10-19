#pragma once

// bw/sleep/episode_classifier.h — F08 §6 D2: the episode classification rule
// table as a pure function: no state, no clock, no I/O, so tests mirror the
// table row by row and the tracker stays free of classification branches.

#include <bw/sleep/sleep_config.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::sleep {

struct ClassifyInput {
    TimeMs duration_ms = 0;                   // episode span, merged gaps included (§6 D1)
    float degraded_share = 0.0f;              // 1 − sleep_like_ms / total_ms, in [0,1] (§6 D5)
    OperatingMode mode = OperatingMode::Day;  // mode of the episode's evidence
    UnixSeconds t_start_unix = 0;             // wall time sampled at candidate start; 0 = unknown
};

class EpisodeClassifier {
public:
    // §6 D2 rule table (first match wins):
    //  1 degraded_share > kMaxDegradedShare     → Uncertain   (bad data beats duration)
    //  2 duration_ms   ≥ kNapMaxMs              → MainSleep   (duration beats mode)
    //  3 mode == Nap (F15 planned-nap intent)   → Nap         (intent beats the clock)
    //  4 t_start_unix == 0 (wall time unknown)  → Uncertain   (never claim day/night)
    //  5 mode == Night                          → Nap
    //  6 otherwise (wall time known, day-ish)   → Rest
    // `cfg` defaults to the §8 constants so §7's one-argument call shape holds
    // while a host test may still vary a single boundary value.
    static EpisodeClass classify(const ClassifyInput& in,
                                 const SleepEpisodeConfig& cfg = SleepEpisodeConfig{});
};

}  // namespace bw::sleep
