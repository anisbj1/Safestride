#pragma once

// firmware/test/bw_confirm/test_bw_confirm/fake_clock.h — scripted-timeline
// clock (plan §7). Tests drive TimeMs explicitly; implements bw::IClock so the
// wire_confirm step(ctx) seam can be exercised on the host too.

#include <bw/clock.h>
#include <bw/types.h>

namespace bw_confirm_test {

class FakeClock : public bw::IClock {
public:
    bw::TimeMs nowMs() const override { return now_ms_; }
    bw::UnixSeconds unixSeconds() const override { return unix_s_; }

    void advance(bw::TimeMs ms) { now_ms_ += ms; }
    void set(bw::TimeMs ms) { now_ms_ = ms; }
    void setUnix(bw::UnixSeconds s) { unix_s_ = s; }

private:
    bw::TimeMs now_ms_ = 0;
    bw::UnixSeconds unix_s_ = 0;
};

}  // namespace bw_confirm_test
