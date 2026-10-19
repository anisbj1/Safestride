#include <bw/signal/ppg_filter_chain.h>

namespace bw::signal {

float PpgFilterChain::Biquad::process(float x) {
    const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

void PpgFilterChain::Biquad::primeSteadyState(float x0) {
    // Direct-Form I steady state for a constant input x0: choose y0 so the
    // difference equation is self-consistent (F02 D7). For a high-pass,
    // b0+b1+b2 = 0 gives y0 = 0 immediately — no startup transient.
    const float num = b0 + b1 + b2;
    const float den = 1.0f + a1 + a2;
    const float y0 = (den != 0.0f) ? (num / den) * x0 : 0.0f;
    x1 = x0;
    x2 = x0;
    y1 = y0;
    y2 = y0;
}

void PpgFilterChain::Biquad::clearState() {
    x1 = 0.0f;
    x2 = 0.0f;
    y1 = 0.0f;
    y2 = 0.0f;
}

void PpgFilterChain::reset() {
    const BiquadCoeffs dc =
        profile_ == PpgProfile::Tester25Hz ? kPpgDcBlockerCoeffsTester : kPpgDcBlockerCoeffs;
    const BiquadCoeffs hp = profile_ == PpgProfile::Tester25Hz ? kPpgBandpassHighpassCoeffsTester
                                                               : kPpgBandpassHighpassCoeffs;
    const BiquadCoeffs lp = profile_ == PpgProfile::Tester25Hz ? kPpgBandpassLowpassCoeffsTester
                                                               : kPpgBandpassLowpassCoeffs;
    dc_blocker_ = {dc.b0, dc.b1, dc.b2, dc.a1, dc.a2, 0, 0, 0, 0};
    bandpass_hp_ = {hp.b0, hp.b1, hp.b2, hp.a1, hp.a2, 0, 0, 0, 0};
    bandpass_lp_ = {lp.b0, lp.b1, lp.b2, lp.a1, lp.a2, 0, 0, 0, 0};
    primed_ = false;
    debug_.clear();
}

float PpgFilterChain::push(uint32_t ir_raw) {
    const float x = static_cast<float>(ir_raw);
    if (!primed_) {
        // Prime ONLY the first section with the raw DC level: in steady
        // state every downstream section sees 0 (both high-passes have zero
        // DC gain), so their steady state is the zero state (F02 D7).
        dc_blocker_.primeSteadyState(x);
        bandpass_hp_.clearState();
        bandpass_lp_.clearState();
        primed_ = true;
    }
    const float y = bandpass_lp_.process(bandpass_hp_.process(dc_blocker_.process(x)));
    // Rolling debug ring for F28: overwriting the oldest point is the design,
    // not a loss (F02 D4).
    (void)debug_.push(PpgFilteredPoint{ir_raw, y});
    return y;
}

size_t PpgFilterChain::copyDebug(PpgFilteredPoint* out, size_t max) const {
    if (out == nullptr) {
        return 0;
    }
    size_t n = debug_.size();
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = *debug_.at(i);  // at(0) is the oldest (bw_core contract)
    }
    return n;
}

}  // namespace bw::signal
