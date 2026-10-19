#pragma once

#include <bw/testkit/host_only.h>

#include <bw/clock.h>
#include <bw/types.h>

namespace bw::testkit {

// FakeClock — scriptable bw::IClock (02 §2) for host tests.
//
// Tests set or advance time explicitly: no wall clock, no sleeps, fully
// (t_ms, input)"). Starts at t = 0 with unixSeconds() == 0 ("unknown", DEC-009) —
// exactly the state a real device is in before the phone's first SetTime.
class FakeClock final : public bw::IClock {
public:
    bw::TimeMs nowMs() const override { return now_ms_; }
    bw::UnixSeconds unixSeconds() const override { return unix_s_; }

    void setMs(bw::TimeMs t) { now_ms_ = t; }
    void advanceMs(bw::TimeMs dt) { now_ms_ += dt; }

    // 0 = wall clock unknown (02 §2); tests that need wall time set a real value.
    void setUnixSeconds(bw::UnixSeconds s) { unix_s_ = s; }

private:
    bw::TimeMs now_ms_ = 0;
    bw::UnixSeconds unix_s_ = 0;
};

}  // namespace bw::testkit
