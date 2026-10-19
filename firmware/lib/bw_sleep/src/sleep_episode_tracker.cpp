// bw/sleep/sleep_episode_tracker.cpp — F08 §6 D1 machine, D4 recovery,
// D5 confidence/degraded share. All time maths go through bw::elapsedMs
// (saturating, bw/time.h) so a stale or pre-reset timestamp can never inflate
// a duration (§6 D1; uint64 TimeMs does not wrap — bw/types.h).

#include <bw/sleep/sleep_episode_tracker.h>

#include <bw/sleep/episode_classifier.h>
#include <bw/time.h>

namespace bw::sleep {

namespace {

// [DERIVED] Bit for a ReasonCode. bw_core defines the enum but no bit helper,
// and features may not include each other's headers (01 §1 rule 1) — every
// feature carries its own tiny helper (bw_state, bw_alert, bw_cataplexy do).
constexpr ReasonMask reasonBit(ReasonCode code) {
    return static_cast<ReasonMask>(1u) << static_cast<uint8_t>(code);
}

// Event::duration_ms is uint32 by contract (02 §4.5) while internal TimeMs is
// uint64 (01 §4: never truncate): saturate instead of silently wrapping. A
// span this large has never been observed in this product's lifetime.
uint32_t saturatingDuration(TimeMs span) {
    constexpr TimeMs kMaxDuration = 0xFFFFFFFFull;
    return span > kMaxDuration ? static_cast<uint32_t>(kMaxDuration) : static_cast<uint32_t>(span);
}

float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    return v > 1.0f ? 1.0f : v;
}

// payload[2]: degraded share as 0–100 % (§6 D7, §8 — percentages are uint8_t
// only inside payload[2]). Share is in [0,1], so rounding half-up saturates at 100.
uint8_t sharePercent(float share) {
    const float pct = share * 100.0f + 0.5f;
    return pct >= 100.0f ? uint8_t{100} : static_cast<uint8_t>(pct);
}

}  // namespace

SleepEpisodeTracker::SleepEpisodeTracker(const SleepEpisodeConfig& cfg, IKeyValueStore* kv,
                                         const IClock* wall_clock)
    : cfg_(cfg), kv_(kv), wall_clock_(wall_clock) {}

// ---- §6 D5 helpers ----------------------------------------------------------

float SleepEpisodeTracker::degradedShare() const {
    if (total_ms_ == 0) {
        // No observed time yet (or a zero-length span): share 0, never NaN (§6 D5).
        return 0.0f;
    }
    const float observed = static_cast<float>(sleep_like_ms_) / static_cast<float>(total_ms_);
    return clamp01(1.0f - observed);
}

float SleepEpisodeTracker::runningConfidence(float degraded_share) const {
    if (snapshot_count_ == 0) {
        return 0.0f;
    }
    const float mean = confidence_sum_ / static_cast<float>(snapshot_count_);
    return clamp01(mean * (1.0f - degraded_share));
}

EpisodeStatus SleepEpisodeTracker::currentStatus(TimeMs t_ms) const {
    EpisodeStatus st{};
    st.t_ms = t_ms;
    st.phase = phase_;
    if (phase_ == EpisodePhase::Idle) {
        // Idle: no episode, so no class claim and no confidence (00 §7.5).
        st.t_start_ms = 0;
        st.episode_class = EpisodeClass::Uncertain;
        st.confidence = 0.0f;
        return st;
    }
    st.t_start_ms = t_start_ms_;
    // Provisional while the episode is open; the close event carries the final
    // class (02 §4.5 EpisodeStatus, §6 D7).
    st.episode_class = EpisodeClassifier::classify(
        {bw::elapsedMs(t_ms, t_start_ms_), degradedShare(), mode_, t_start_unix_}, cfg_);
    st.confidence = runningConfidence(degradedShare());
    return st;
}

// ---- §6 D4 marker -----------------------------------------------------------

void SleepEpisodeTracker::writeMarker() {
    if (kv_ == nullptr) {
        return;
    }
    OpenMarkerRecord marker{};
    marker.schema_version = cfg_.kMarkerSchemaVersion;
    marker.t_start_unix = t_start_unix_;
    // §10: a failed write at promotion degrades recovery only — the episode
    // still becomes Active and events keep flowing.
    const Result<void> wrote = kv_->put(cfg_.kMarkerKey, &marker, sizeof(marker));
    (void)wrote;
}

void SleepEpisodeTracker::clearMarker() {
    if (kv_ == nullptr) {
        return;
    }
    // IKeyValueStore has no remove (02 §5): "cleared" is a schema-0 record,
    // which recover() ignores by construction.
    OpenMarkerRecord cleared{};
    const Result<void> result = kv_->put(cfg_.kMarkerKey, &cleared, sizeof(cleared));
    (void)result;
}

// ---- events ------------------------------------------------------------------

void SleepEpisodeTracker::fillEventShell(Event& e, const StateSnapshot& s) const {
    e = Event{};  // zero-init: unused payload bytes and F17-owned fields stay 0
    e.type = EventType::SleepEpisode;
    e.context = activity_;  // the context the episode started in (§6 D1 row 2)
    e.alertness_at_start = AlertnessState::PossibleSleep;  // the state that started it
    e.quality = s.quality;  // per-source quality of the triggering observation
    e.detector_id = cfg_.kSleepEpisodeDetectorId;
    e.schema_version = Event::kSchemaVersion;
}

void SleepEpisodeTracker::promote(StepOutput& out, const StateSnapshot& s, TimeMs span) {
    const float share = degradedShare();
    // Provisional class (§6 D7): the open marker carries the best guess at
    // promotion time; the close event carries the final one.
    const EpisodeClass provisional =
        EpisodeClassifier::classify({span, share, mode_, t_start_unix_}, cfg_);

    phase_ = EpisodePhase::Active;
    gap_ms_ = 0;  // promotion happens on a sleep-like snapshot: no gap is open
    mode_ = s.mode;

    // Open-marker event (§3, §6 D1 row 3, §6 D7 layout): duration-so-far lets
    // a consumer pair it with the eventual close.
    Event& e = out.event;
    fillEventShell(e, s);
    e.t_start_ms = t_start_ms_;
    e.duration_ms = saturatingDuration(span);
    e.t_start_unix = t_start_unix_;
    e.confidence = runningConfidence(share);
    e.reasons = 0;
    e.payload[SleepEpisodeConfig::kPayloadFlagsIndex] = cfg_.kFlagOpenMarker;
    e.payload[SleepEpisodeConfig::kPayloadClassIndex] = static_cast<uint8_t>(provisional);
    e.payload[SleepEpisodeConfig::kPayloadShareIndex] = sharePercent(share);
    out.has_event = true;

    // §6 D4: the one durable write per episode — never at Candidate, so blips
    // cost no flash writes. kv_ == nullptr (the W2 build) skips silently.
    writeMarker();
    out.status_changed = true;
}

void SleepEpisodeTracker::closeActive(StepOutput& out, TimeMs t_close_ms, const StateSnapshot& s,
                                      bool force_uncertain, ReasonMask extra_reasons) {
    const TimeMs span = bw::elapsedMs(t_close_ms, t_start_ms_);
    const float share = degradedShare();
    // Row 8 forces Uncertain: the tail of the episode happened where this boot
    // cannot see it (§6 D1, §10). Normal closes run the §6 D2 rule table.
    const EpisodeClass cls =
        force_uncertain ? EpisodeClass::Uncertain
                        : EpisodeClassifier::classify({span, share, mode_, t_start_unix_}, cfg_);

    Event& e = out.event;
    fillEventShell(e, s);
    e.t_start_ms = t_start_ms_;
    e.duration_ms = saturatingDuration(span);
    e.t_start_unix = t_start_unix_;
    e.confidence = runningConfidence(share);
    e.reasons = extra_reasons;
    // Closed: neither open nor recovered — a zeroed payload decodes as
    // Uncertain/0 %, the honest default (§6 D7).
    e.payload[SleepEpisodeConfig::kPayloadFlagsIndex] = 0;
    e.payload[SleepEpisodeConfig::kPayloadClassIndex] = static_cast<uint8_t>(cls);
    e.payload[SleepEpisodeConfig::kPayloadShareIndex] = sharePercent(share);
    out.has_event = true;

    clearMarker();  // §6 D4: the episode is over; a stale marker would re-trigger recovery (§10)
    resetEpisode();
    out.status_changed = true;
}

void SleepEpisodeTracker::resetEpisode() {
    phase_ = EpisodePhase::Idle;
    t_start_ms_ = 0;
    gap_ms_ = 0;
    t_start_unix_ = 0;
    mode_ = OperatingMode::Day;
    activity_ = ActivityContext::Unknown;
    sleep_like_ms_ = 0;
    total_ms_ = 0;
    confidence_sum_ = 0.0f;
    snapshot_count_ = 0;
    // last_t_ms_/have_last_t_ survive: row-1 de-duplication and row-8
    // discontinuity detection apply across episode boundaries too.
}

// ---- §6 D1 state machine -----------------------------------------------------

StepOutput SleepEpisodeTracker::step(const StateSnapshot& s) {
    StepOutput out{};

    // Row 1: the wiring polls IStateProvider::latest() every tick; a snapshot
    // that repeats the previous t_ms adds no information — no-op.
    if (have_last_t_ && s.t_ms == last_t_ms_) {
        out.status = currentStatus(s.t_ms);
        return out;
    }

    // Row 8: t_ms went backwards — a reboot/counter discontinuity. An open
    // episode closes on the LAST COHERENT timestamp, class Uncertain, reason
    // SensorUnavailable (§6 D1, §10): never build a duration across the
    // discontinuity. A Candidate was never promoted, so — like row 4 — it
    // publishes nothing. In Idle there is nothing to corrupt; adopt the new
    // timeline (elapsedMs saturates anyway, §6 D1).
    if (have_last_t_ && s.t_ms < last_t_ms_) {
        if (phase_ == EpisodePhase::Active) {
            closeActive(out, last_t_ms_, s, /*force_uncertain=*/true,
                        reasonBit(ReasonCode::SensorUnavailable));
        } else if (phase_ == EpisodePhase::Candidate) {
            resetEpisode();
            out.status_changed = true;
        }
        last_t_ms_ = s.t_ms;
        out.status = currentStatus(s.t_ms);
        return out;
    }

    // Δt from the previous observation (0 on the very first snapshot).
    const TimeMs dt = have_last_t_ ? bw::elapsedMs(s.t_ms, last_t_ms_) : TimeMs{0};
    // "Sleep-like" per §6 D1's gap definition: PossibleSleep with both sources
    // usable. Unknown/NotWorn/Insufficient are ordinary gap time — no pause
    // state (§6 D1 choice); time not observed as sleep counts as degraded (§6 D5).
    const bool sleep_like = (s.alertness == AlertnessState::PossibleSleep) &&
                            (s.quality.imu != DataQuality::Insufficient) &&
                            (s.quality.ppg != DataQuality::Insufficient);

    last_t_ms_ = s.t_ms;
    have_last_t_ = true;

    if (phase_ == EpisodePhase::Idle) {
        if (sleep_like) {  // row 2: PossibleSleep starts a candidate
            t_start_ms_ = s.t_ms;
            // Wall time sampled ONCE, at candidate start (§6 D2); nullptr clock
            // or an unsynced device clock (unixSeconds() == 0, DEC-009) both
            // stay "unknown" — never fabricated later.
            t_start_unix_ = (wall_clock_ != nullptr) ? wall_clock_->unixSeconds() : UnixSeconds{0};
            mode_ = s.mode;
            activity_ = s.activity;
            total_ms_ = 0;
            sleep_like_ms_ = 0;
            confidence_sum_ = s.confidence;
            snapshot_count_ = 1;
            gap_ms_ = 0;
            phase_ = EpisodePhase::Candidate;
            out.status_changed = true;
        }
        out.status = currentStatus(s.t_ms);
        return out;
    }

    // Candidate or Active: this observation joins the episode's time budget
    // (§6 D5). Time between sleep-like snapshots (gaps, NotWorn, Unknown) is
    // degraded — honest, never invented. A row-4/6 abort wipes the accumulators.
    total_ms_ += dt;
    confidence_sum_ += s.confidence;
    ++snapshot_count_;
    if (sleep_like) {
        sleep_like_ms_ += dt;
    }

    // Gap accounting (§6 D1): a gap is OBSERVED non-PossibleSleep time — the
    // snapshots whose alertness is not PossibleSleep. A stale (silent) stretch
    // produces no snapshots and is therefore NOT a gap: it stays degraded time
    // in the §6 D5 accumulators above, while NotWorn/Unknown/Insufficient
    // snapshots are ordinary observed gap time (row 4's "any reason"). An
    // in-episode sleep-like snapshot ends any ongoing gap.
    if (!sleep_like) {
        gap_ms_ += dt;
    }

    if (phase_ == EpisodePhase::Candidate) {
        if (gap_ms_ > cfg_.kMergeGapMs) {
            // Row 4: observed wakefulness beyond the merge budget, for any
            // reason — drowsiness/inactivity, not an episode, so nothing is
            // published (§6 D1).
            resetEpisode();
            out.status_changed = true;
        } else if (sleep_like && bw::elapsedMs(s.t_ms, t_start_ms_) >= cfg_.kMinEpisodeMs) {
            // Row 3 (inclusive boundary): the sleep-like EVIDENCE reached
            // kMinEpisodeMs; promotion requires a sleep-like snapshot so an
            // observed wake blip can never open a marker, and rows 4/6 have
            // already bounded every gap by kMergeGapMs.
            promote(out, s, bw::elapsedMs(s.t_ms, t_start_ms_));
        } else if (sleep_like) {
            // Candidate continues; merged gaps are already span time (§6 D1).
            gap_ms_ = 0;
            mode_ = s.mode;
        }
    } else {  // EpisodePhase::Active
        if (gap_ms_ > cfg_.kMergeGapMs) {
            // Rows 6/7: close after sustained observed wakefulness.
            // DeviceNotWorn explains a removal; the observed part is kept
            // (§13, closed in D1).
            closeActive(out, s.t_ms, s, /*force_uncertain=*/false,
                        (s.wear == WearState::NotWorn) ? reasonBit(ReasonCode::DeviceNotWorn)
                                                       : ReasonMask{0});
        } else if (sleep_like) {
            // Row 5: the episode continues (merged gap or still sleep-like).
            gap_ms_ = 0;
            mode_ = s.mode;
        }
    }

    out.status = currentStatus(s.t_ms);
    return out;
}

// ---- §6 D4 recovery ----------------------------------------------------------

StepOutput SleepEpisodeTracker::recover() {
    StepOutput out{};
    out.status = currentStatus(last_t_ms_);
    // §6 D4/§9: exactly one recovery per boot, even when called twice.
    if (recovered_) {
        return out;
    }
    recovered_ = true;
    // No store wired is the wave-2 configuration and the normal case, not an
    // error (§10): no marker, no event, no fault.
    if (kv_ == nullptr) {
        return out;
    }
    OpenMarkerRecord marker{};
    const Result<size_t> got = kv_->get(cfg_.kMarkerKey, &marker, sizeof(marker));
    if (!got.ok() || got.value != sizeof(marker) ||
        marker.schema_version != cfg_.kMarkerSchemaVersion) {
        // Absent, truncated or corrupt: "ignore" instead of decoding garbage
        // (§6 D4, §8). A missing marker is the normal case (§10).
        return out;
    }
    // Consumers learn exactly one fact (§6 D4): a sleep-like episode was open
    // before the reboot; start and duration are unknown. The marker's
    // monotonic times belonged to the previous boot — reusing them under the
    // new boot_id would be a lie.
    Event& e = out.event;
    e = Event{};
    e.type = EventType::SleepEpisode;
    e.t_start_ms = 0;
    e.duration_ms = 0;
    e.t_start_unix = marker.t_start_unix;  // 0 if the device never synced
    e.confidence = 0.0f;
    e.reasons = reasonBit(ReasonCode::SensorUnavailable);
    e.payload[SleepEpisodeConfig::kPayloadFlagsIndex] = cfg_.kFlagRecovered;
    e.payload[SleepEpisodeConfig::kPayloadClassIndex] =
        static_cast<uint8_t>(EpisodeClass::Uncertain);
    // 100 % of the episode is unobserved from this boot — honestly degraded,
    // never a fabricated "clean" 0 % (00 §7.5).
    e.payload[SleepEpisodeConfig::kPayloadShareIndex] = 100;
    e.quality = {DataQuality::Insufficient, DataQuality::Insufficient};
    e.context = ActivityContext::Unknown;
    e.alertness_at_start = AlertnessState::PossibleSleep;
    e.detector_id = cfg_.kSleepEpisodeDetectorId;
    e.schema_version = Event::kSchemaVersion;
    out.has_event = true;
    clearMarker();  // §9: the marker is cleared after recover()
    return out;
}

}  // namespace bw::sleep
