// bw/sleep/episode_classifier.cpp — F08 §6 D2 implemented verbatim.
//
// Ordering is deliberate: duration beats mode (a 4 h planned nap is recorded
// as MainSleep) and intent beats the clock (a planned nap needs no sync). Each
// clause below cites its table row so spec edits are traceable here.

#include <bw/sleep/episode_classifier.h>

namespace bw::sleep {

EpisodeClass EpisodeClassifier::classify(const ClassifyInput& in, const SleepEpisodeConfig& cfg) {
    // Row 1: more degraded data than the episode can carry — nothing reliable
    // to classify, and "Uncertain" is a first-class result, not a failure
    // (00 §7.5). `>` is inclusive-at-the-limit: exactly kMaxDegradedShare passes.
    if (in.degraded_share > cfg.kMaxDegradedShare) {
        return EpisodeClass::Uncertain;
    }
    // Row 2: duration beats mode — a 4 h planned nap is still main sleep.
    if (in.duration_ms >= cfg.kNapMaxMs) {
        return EpisodeClass::MainSleep;
    }
    // Row 3: F15's planned-nap signal — user intent, no clock sync required.
    if (in.mode == OperatingMode::Nap) {
        return EpisodeClass::Nap;
    }
    // Row 4: wall time unknown (DEC-009) — F08 never claims a day/night axis
    // it does not have; the honest output is Uncertain.
    if (in.t_start_unix == 0) {
        return EpisodeClass::Uncertain;
    }
    // Row 5: short night fragment — nap-shaped sleep, not a daytime rest.
    if (in.mode == OperatingMode::Night) {
        return EpisodeClass::Nap;
    }
    // Row 6: wall time known, day-ish mode (Day/Calibration/Research) — an
    // ordinary rest span.
    return EpisodeClass::Rest;
}

}  // namespace bw::sleep
