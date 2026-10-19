// bw/diag/ppg_quality_assessor.cpp — F23 PPG diagnosis. See header.

#include <bw/diag/ppg_quality_assessor.h>

namespace bw::diag {

bool PpgQualityAssessor::sameDc(const PpgFeatures& a, const PpgFeatures& b) {
    // Bit-equal DC level: the deterministic pipeline maps identical raw IR
    // to identical ir_dc_level (D6a reasoning).
    return a.ir_dc_level == b.ir_dc_level;
}

PpgRawEval PpgQualityAssessor::evaluatePpgRawWindow(const PpgSample* samples, size_t n) const {
    PpgRawEval eval{};
    if (samples == nullptr || n == 0) {
        // Not evaluated (D6): fail-safe, never a guess.
        eval.instantaneous = DataQuality::Insufficient;
        return eval;
    }

    size_t saturated_count = 0;
    uint32_t run = 1;
    uint32_t longest_run = 1;
    for (size_t i = 0; i < n; ++i) {
        if (samples[i].saturated) {
            ++saturated_count;
        }
        if (i > 0 && samples[i].ir == samples[i - 1].ir) {
            ++run;
            if (run > longest_run) {
                longest_run = run;
            }
        } else {
            run = 1;
        }
    }

    // Saturation first: clipped windows are unusable whatever the run
    // lengths say (the clipped value itself can fake a flat-line).
    const float saturated_fraction = static_cast<float>(saturated_count) / static_cast<float>(n);
    if (saturated_fraction >= config_.saturated_window_fraction) {
        eval.instantaneous = DataQuality::Insufficient;
        eval.edge = FaultEdge{FaultCode::PpgSaturated, FaultSeverity::Error};
        return eval;
    }
    if (longest_run >= config_.ppg_flatline_raw_samples) {
        eval.instantaneous = DataQuality::Insufficient;
        eval.edge = FaultEdge{FaultCode::PpgFlatline, FaultSeverity::Error};
        return eval;
    }
    eval.instantaneous = DataQuality::Good;
    return eval;
}

PpgAssessOutput PpgQualityAssessor::step(TimeMs now_ms, bool has_status, SensorStatus status,
                                         const PpgFeatures* features, const PpgSample* raw,
                                         size_t raw_n) {
    PpgAssessOutput out{};

    // Missing / dead sensor: quality only, F01 owns the codes (D6).
    if (!has_status || status == SensorStatus::Failed || status == SensorStatus::Uninitialized) {
        reset();  // sensor absence breaks both raw and feature continuity
        out.instantaneous = DataQuality::Insufficient;
        return out;
    }

    const bool has_raw = (raw != nullptr && raw_n > 0);
    PpgRawEval raw_eval{};
    if (has_raw) {
        raw_eval = evaluatePpgRawWindow(raw, raw_n);
        bool flatline_in_window = false;
        for (size_t i = 0; i < raw_n; ++i) {
            const uint32_t previous_run = raw_run_;
            if (has_raw_ir_ && raw[i].ir == raw_ir_) {
                if (raw_run_ < UINT32_MAX) {
                    ++raw_run_;
                }
            } else {
                raw_run_ = 1;
            }
            raw_ir_ = raw[i].ir;
            has_raw_ir_ = true;
            flatline_in_window =
                flatline_in_window || (previous_run < config_.ppg_flatline_raw_samples &&
                                       raw_run_ >= config_.ppg_flatline_raw_samples);
        }
        if ((flatline_in_window || raw_run_ >= config_.ppg_flatline_raw_samples) &&
            raw_eval.edge.code != FaultCode::PpgSaturated) {
            raw_eval.instantaneous = DataQuality::Insufficient;
            raw_eval.edge = FaultEdge{FaultCode::PpgFlatline, FaultSeverity::Error};
        }
    } else {
        has_raw_ir_ = false;
        raw_run_ = 0;
    }
    const bool raw_flatline = has_raw && raw_eval.edge.code == FaultCode::PpgFlatline;
    const bool raw_saturated = has_raw && raw_eval.edge.code == FaultCode::PpgSaturated;

    // Feature frames: absent/stale ⇒ fail-safe Insufficient (§15.4) — but
    // an evaluated raw window is real evidence of the current stream.
    const bool fresh =
        features != nullptr && now_ms >= features->t_ms &&
        (now_ms - features->t_ms) <= static_cast<TimeMs>(config_.feature_staleness_ms);

    bool dc_flatline = false;
    bool poor_contact = false;
    if (fresh) {
        // Identical-DC run (D6a).
        if (!has_prev_frame_ || features->t_ms != prev_frame_.t_ms) {
            if (has_prev_frame_ && features->t_ms > prev_frame_.t_ms &&
                sameDc(*features, prev_frame_)) {
                if (dc_run_ < UINT8_MAX) {
                    ++dc_run_;
                }
            } else {
                dc_run_ = 1;
            }
            prev_frame_ = *features;
            has_prev_frame_ = true;
        }

        const bool low_perfusion = features->perfusion_index < config_.poor_contact_perfusion_index;
        // Flat-line needs BOTH a dead DC and no pulsation: a constant DC
        // with healthy perfusion is a legitimate constant offset, not a
        // dead sensor (§15.3 row PpgFlatline).
        dc_flatline = dc_run_ >= config_.stuck_feature_windows && low_perfusion;
        poor_contact = !dc_flatline && low_perfusion;
    } else {
        has_prev_frame_ = false;
        dc_run_ = 0;
    }
    flatline_latched_ = raw_flatline || dc_flatline;
    saturated_latched_ = raw_saturated;
    poor_contact_latched_ = poor_contact;

    // --- quality -------------------------------------------------------------
    // F01's driver hint is a floor for BOTH sources (§15.4): Degraded
    // status can only lower the result — the same worst-of mapping the
    // IMU assessor applies.
    const DataQuality status_hint =
        (status == SensorStatus::Degraded) ? DataQuality::Degraded : DataQuality::Good;
    DataQuality own = status_hint;
    if (!fresh) {
        if (!has_raw) {
            own = DataQuality::Insufficient;  // fail-safe dominates the hint
        } else if (static_cast<uint8_t>(raw_eval.instantaneous) > static_cast<uint8_t>(own)) {
            own = raw_eval.instantaneous;
        }
    } else {
        if (features->motion_corrupted) {
            own = DataQuality::Degraded;  // cross-modal hint: window too energetic
        }
        if (poor_contact_latched_) {
            own = DataQuality::Degraded;
        }
        if (dc_flatline) {
            own = DataQuality::Insufficient;
        }
        // Worst-of with F02's own quality hint (§15.3 row PpgPoorContact).
        if (static_cast<uint8_t>(features->quality) > static_cast<uint8_t>(own)) {
            own = features->quality;
        }
    }
    out.instantaneous = own;
    if (raw_flatline || raw_saturated) {
        out.instantaneous = DataQuality::Insufficient;  // raw evidence beats all
    }

    // --- edges: Saturated > Flatline > PoorContact ----------------------------
    if (saturated_latched_ && !saturated_edge_emitted_) {
        out.edge = FaultEdge{FaultCode::PpgSaturated, FaultSeverity::Error};
        saturated_edge_emitted_ = true;
    } else if (flatline_latched_ && !flatline_edge_emitted_) {
        out.edge = FaultEdge{FaultCode::PpgFlatline, FaultSeverity::Error};
        flatline_edge_emitted_ = true;
    } else if (poor_contact_latched_ && !poor_contact_edge_emitted_) {
        out.edge = FaultEdge{FaultCode::PpgPoorContact, FaultSeverity::Warning};
        poor_contact_edge_emitted_ = true;
    }

    // Release the emission flags of episodes that have cleared, so the
    // next episode edges again (subject to Diagnostics' rate limit, D9).
    if (!saturated_latched_) {
        saturated_edge_emitted_ = false;
    }
    if (!flatline_latched_) {
        flatline_edge_emitted_ = false;
    }
    if (!poor_contact_latched_) {
        poor_contact_edge_emitted_ = false;
    }
    return out;
}

void PpgQualityAssessor::reset() {
    has_raw_ir_ = false;
    raw_run_ = 0;
    has_prev_frame_ = false;
    dc_run_ = 0;
    flatline_latched_ = false;
    flatline_edge_emitted_ = false;
    poor_contact_latched_ = false;
    poor_contact_edge_emitted_ = false;
    saturated_latched_ = false;
    saturated_edge_emitted_ = false;
}

}  // namespace bw::diag
