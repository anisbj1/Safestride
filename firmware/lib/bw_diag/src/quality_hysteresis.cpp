// bw/diag/quality_hysteresis.cpp — F23 D7 implementation. See header.

#include <bw/diag/quality_hysteresis.h>

namespace bw::diag {

bool QualityHysteresis::isBetter(DataQuality to, DataQuality from) {
    return static_cast<uint8_t>(to) < static_cast<uint8_t>(from);
}

DataQuality QualityHysteresis::update(DataQuality instantaneous) {
    // D7 fail-safe asymmetry: Insufficient is honest NOW, no dwell.
    if (instantaneous == DataQuality::Insufficient) {
        current_ = DataQuality::Insufficient;
        run_level_ = DataQuality::Good;
        run_count_ = 0;
        return current_;
    }

    // Consecutive-run counter: a different judgment restarts the run, so
    // flapping noise can never accumulate toward either direction.
    if (instantaneous != run_level_) {
        run_level_ = instantaneous;
        run_count_ = 1;
    } else if (run_count_ < UINT8_MAX) {
        ++run_count_;
    }

    if (instantaneous != current_) {
        // Moving up (to a better level) pays the slow-recovery price;
        // moving down (Degraded) pays only the enter dwell.
        const uint8_t need = isBetter(instantaneous, current_) ? config_.good_recover_windows
                                                               : config_.degraded_enter_windows;
        if (run_count_ >= need) {
            current_ = instantaneous;
        }
    }
    return current_;
}

void QualityHysteresis::reset() {
    current_ = DataQuality::Insufficient;
    run_level_ = DataQuality::Good;
    run_count_ = 0;
}

}  // namespace bw::diag
