#include <bw/hal/adc_battery_monitor.h>

#if defined(ARDUINO)

#include <Arduino.h>

namespace bw::hal {

void AdcBatteryMonitor::begin() {
    pinMode(adc_pin_, INPUT);
    // [HW] 11 dB attenuation ≈ 0–3.1 V full scale (esp32-hal-adc.h): enough
    // for a divided battery (4.2 V / divider_ratio). The core already
    // defaults every pin to ADC_ATTENDB_MAX (esp32-hal-adc.c) — setting it
    // explicitly documents the range the divider math assumes instead of
    // inheriting a default silently.
    analogSetPinAttenuation(static_cast<uint8_t>(adc_pin_), ADC_11db);
    if (!cfg_.divider_present && !unavailable_reported_ && faults_ != nullptr) {
        // A-04: no divider is wired. One Info report per boot (D7 edge rule):
        // the value is "unknown", the device is not broken — battery stays a
        // validity flag, never a fabricated voltage (02 §1).
        unavailable_reported_ = true;
        const bw::TimeMs t_ms = (clock_ != nullptr) ? clock_->nowMs() : 0;
        faults_->report(bw::FaultCode::BatterySenseUnavailable, bw::FaultSeverity::Info, t_ms);
    }
}

bw::BatteryReading AdcBatteryMonitor::read() {
    if (!cfg_.divider_present) {
        // Do not touch a pin with no divider on it: a floating ADC would
        // invent a voltage. batteryFromAdc returns valid = false — "unknown".
        return batteryFromAdc(0, cfg_);
    }
    uint32_t sum = 0;
    for (uint16_t i = 0; i < kBatteryAdcSampleCount; ++i) {
        sum += static_cast<uint16_t>(analogRead(static_cast<uint8_t>(adc_pin_)));
    }
    // Integer mean: the residual <= 0.5 count is far below the ADC's own
    // noise and non-linearity, so no float accumulation in a read path.
    const uint16_t counts = static_cast<uint16_t>(sum / kBatteryAdcSampleCount);
    return batteryFromAdc(counts, cfg_);
}

}  // namespace bw::hal

#endif  // ARDUINO
