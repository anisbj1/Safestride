#include <bw/signal/tester_heart_rate.h>

namespace bw::signal {

void TesterHeartRate::resetBeats() {
    rising_ = false;
    have_peak_ = false;
    last_peak_ms_ = 0;
    last_valid_ms_ = 0;
    index_ = 0;
    count_ = 0;
    bpm_ = 0.0f;
    for (float& value : history_) {
        value = 0.0f;
    }
}

void TesterHeartRate::updateTime(TimeMs now_ms) {
    if (count_ != 0 && now_ms >= last_valid_ms_ && now_ms - last_valid_ms_ > kTesterBpmStaleMs) {
        resetBeats();
    }
}

bool TesterHeartRate::push(const PpgSample& sample) {
    updateTime(sample.t_ms);
    // Saturated input cannot establish a trustworthy pulse. Contact loss must
    // discard the average, not carry an old number into a new finger placement.
    if (sample.ir < kTesterContactIr || sample.saturated) {
        contact_ = false;
        last_ir_ = sample.ir;
        derivative_ = 0.0f;
        resetBeats();
        return false;
    }
    if (!contact_) {
        contact_ = true;
        last_ir_ = sample.ir;
        derivative_ = 0.0f;
        resetBeats();
        return false;  // initial placement is not a heartbeat
    }
    const float difference =
        static_cast<float>(static_cast<int32_t>(sample.ir) - static_cast<int32_t>(last_ir_));
    last_ir_ = sample.ir;
    derivative_ =
        (1.0f - kTesterDerivativeAlpha) * derivative_ + kTesterDerivativeAlpha * difference;
    if (derivative_ > kTesterDerivativeArm) {
        rising_ = true;
    }
    if (!rising_ || derivative_ >= kTesterDerivativeFall) {
        return false;
    }
    rising_ = false;
    if (!have_peak_ || sample.t_ms < last_peak_ms_) {
        have_peak_ = true;
        last_peak_ms_ = sample.t_ms;
        return false;
    }
    const TimeMs interval = sample.t_ms - last_peak_ms_;
    if (interval > kTesterMaxIbiMs) {
        last_peak_ms_ = sample.t_ms;
        return false;
    }
    if (interval < kTesterMinIbiMs) {
        return false;
    }
    const float instant_bpm = static_cast<float>(kMsPerMinute) / static_cast<float>(interval);
    if (instant_bpm < kTesterMinBpm || instant_bpm > kTesterMaxBpm) {
        return false;
    }
    history_[index_] = instant_bpm;
    index_ = (index_ + 1) % kTesterBpmHistorySize;
    if (count_ < kTesterBpmHistorySize) {
        ++count_;
    }
    float sum = 0.0f;
    for (size_t i = 0; i < count_; ++i) {
        sum += history_[i];
    }
    bpm_ = sum / static_cast<float>(count_);
    last_peak_ms_ = sample.t_ms;
    last_valid_ms_ = sample.t_ms;
    return true;
}

}  // namespace bw::signal
