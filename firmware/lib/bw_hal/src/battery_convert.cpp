#include <bw/hal/battery_convert.h>

namespace bw::hal {

BatteryReading batteryFromAdc(uint16_t counts, const BatteryConfig& cfg) {
    BatteryReading reading{};
    reading.valid = false;
    reading.voltage_v = 0.0f;
    reading.percent_estimate = 0.0f;
    reading.charging_known = cfg.charging_known;
    reading.charging = false;  // never true while A-06 leaves status unwired

    if (!cfg.divider_present || cfg.adc_full_scale == 0) {
        // A-04: no divider proven at the bench -> battery level is "unknown".
        // A zero voltage here is the validity-flag sentinel's companion, not
        // a measurement (02 §1: not-a-number via flag, never a magic value).
        return reading;
    }

    const float volts = (static_cast<float>(counts) / static_cast<float>(cfg.adc_full_scale)) *
                        cfg.vref_v * cfg.divider_ratio * cfg.calibration;
    reading.voltage_v = volts;

    const float span = cfg.v_full_v - cfg.v_empty_v;
    float percent = 0.0f;
    if (span > 0.0f) {
        percent = (volts - cfg.v_empty_v) / span * 100.0f;
    }
    if (percent < 0.0f) {
        percent = 0.0f;
    } else if (percent > 100.0f) {
        percent = 100.0f;
    }
    reading.percent_estimate = percent;
    reading.valid = true;
    return reading;
}

}  // namespace bw::hal
