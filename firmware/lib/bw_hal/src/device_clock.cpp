#include <bw/hal/device_clock.h>

#if defined(ARDUINO)

#include <esp_timer.h>

namespace bw::hal {

namespace {
// [DERIVED] Unit conversion: esp_timer counts microseconds, the contract unit
// is TimeMs milliseconds (02 §1).
constexpr int64_t kMicrosecondsPerMillisecond = 1000;
}  // namespace

bw::TimeMs DeviceClock::nowMs() const {
    return static_cast<bw::TimeMs>(esp_timer_get_time() / kMicrosecondsPerMillisecond);
}

}  // namespace bw::hal

#endif  // ARDUINO
