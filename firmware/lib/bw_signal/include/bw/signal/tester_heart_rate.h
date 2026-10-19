#pragma once

#include <bw/hardware.h>
#include <bw/signal/signal_config.h>

namespace bw::signal {

// Pure port of the developer's independent MAX30102 tester detector. Time is
// the sample timestamp, not task execution time; updateTime also expires a
// stopped stream when no new samples arrive. No sensor/UI/platform dependency.
class TesterHeartRate {
public:
    bool push(const PpgSample& sample);
    void updateTime(TimeMs now_ms);
    bool valid() const { return contact_ && count_ != 0; }
    float bpm() const { return valid() ? bpm_ : 0.0f; }
    bool contact() const { return contact_; }
    float derivative() const { return derivative_; }

private:
    void resetBeats();
    uint32_t last_ir_ = 0;
    float derivative_ = 0.0f;
    bool contact_ = false;
    bool rising_ = false;
    bool have_peak_ = false;
    TimeMs last_peak_ms_ = 0;
    TimeMs last_valid_ms_ = 0;
    float history_[kTesterBpmHistorySize]{};
    size_t index_ = 0;
    size_t count_ = 0;
    float bpm_ = 0.0f;
};

}  // namespace bw::signal
