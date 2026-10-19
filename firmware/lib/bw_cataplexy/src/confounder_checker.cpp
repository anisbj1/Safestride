// bw/cataplexy/confounder_checker.cpp — F09 §7 ConfounderChecker (spec §6 D2).

#include <bw/cataplexy/confounder_checker.h>

#include <cmath>

namespace bw::cataplexy {
namespace {

// A sample only objects if it describes a usable window: a missing earlier
// window is not evidence of a short burst, it is no evidence at all.
bool isUsable(const MotionSample& sample) {
    return sample.valid;
}

}  // namespace

ConfounderMask checkConfounders(const ConfounderInput& input, const CataplexyConfig& config) {
    ConfounderMask mask = 0;

    if (!input.has_previous || !isUsable(input.previous)) {
        // The rule only runs with a usable active window (the detector checks
        // this first), so this is defensive: no objections, no confounders.
        return mask;
    }

    // Fall: the descent into stillness was uncontrolled — either a jerk spike
    // or an impact-sized excess over gravity inside the active window.
    if (input.previous.jerk_rms_g_per_s >= config.fall_jerk_g_per_s ||
        input.previous.enmo_g >= config.fall_peak_enmo_g) {
        mask = static_cast<ConfounderMask>(mask | confounderBit(Confounder::FallLike));
    }

    // Posture change: the wrist's gravity angle moved while the arm dropped
    // (sitting down, laying the arm down) instead of staying where it was.
    if (std::fabs(input.previous.tilt_deg - input.current.tilt_deg) >=
        config.posture_tilt_delta_deg) {
        mask = static_cast<ConfounderMask>(mask | confounderBit(Confounder::PostureChange));
    }

    // Brief burst: the window before the active one does not show sustained
    // activity, so the motion that ended was a short gesture, not an ongoing
    // action that stopped abruptly.
    if (!input.has_earlier || !isUsable(input.earlier) ||
        input.earlier.enmo_g < config.active_motion_enmo_g) {
        mask = static_cast<ConfounderMask>(mask | confounderBit(Confounder::BriefBurst));
    }

    return mask;
}

}  // namespace bw::cataplexy
