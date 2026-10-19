#pragma once

// bw/hal/device_clock.h — the device implementation of bw::IClock (02 §2).
// Platform-only file (F01 D5): the class exists only where ARDUINO is defined.
//
// F00 §13 left this open ("F01 or the integrator must supply it before M1",
// app.h). F01 takes it: reading hardware time is a HAL concern and no other
// feature may touch hardware (01 §1). See F01 spec D9.

#if defined(ARDUINO)

#include <bw/clock.h>
#include <bw/types.h>

namespace bw::hal {

// Monotonic ms since boot from esp_timer (ESP-IDF: int64 microseconds since
// boot — no 32-bit wrap at 49.7 days, no accumulator state). unixSeconds()
// returns 0 by contract: wall time is phone-provided and unknown until first
// sync (01 §4); the sync feature owns setting it later, not the HAL.
class DeviceClock final : public bw::IClock {
public:
    bw::TimeMs nowMs() const override;
    bw::UnixSeconds unixSeconds() const override { return 0; }
};

}  // namespace bw::hal

#endif  // ARDUINO
