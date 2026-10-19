#pragma once

// bw/hal/adc_battery_monitor.h — battery voltage sense behind
// bw::IBatteryMonitor (F01). Platform-only file (F01 D5): samples the ADC,
// then delegates every conversion decision to the pure battery_convert module
// (host-tested divider math, clamping, A-04 "no divider -> unknown").
// No thresholds for decisions here (spec §5): percent is a coarse estimate,
// F23 decides anything about low battery.

#if defined(ARDUINO)

#include <bw/clock.h>
#include <bw/fault.h>
#include <bw/hal/battery_convert.h>
#include <bw/hardware.h>

namespace bw::hal {

// [EXPERIMENTAL] Samples averaged per read(). Why 8: ESP32 single ADC
// conversions are noisy and non-linear (datasheet INL up to a few percent);
// averaging 8 cuts random noise by ~sqrt(8) while costing well under 1 ms at
// the <= 1 Hz rate F23 is expected to call this (a power of two keeps the
// integer division exact).
constexpr uint16_t kBatteryAdcSampleCount = 8;

class AdcBatteryMonitor final : public bw::IBatteryMonitor {
public:
    // `faults`/`clock` may be nullptr (F01 D7/D9 fallbacks). `cfg` is built
    // from config/hw_config.h at wiring time.
    AdcBatteryMonitor(int adc_pin, const BatteryConfig& cfg, bw::IFaultReporter* faults,
                      const bw::IClock* clock)
        : adc_pin_(adc_pin), cfg_(cfg), faults_(faults), clock_(clock) {}

    // Pin setup + A-04: when no divider exists, reports BatterySenseUnavailable
    // once (Info — the value is "unknown", the device is not broken) so the
    // absence is visible in diagnostics without pretending to a measurement.
    void begin();

    bw::BatteryReading read() override;

private:
    int adc_pin_;
    BatteryConfig cfg_;
    bw::IFaultReporter* faults_;
    const bw::IClock* clock_;
    bool unavailable_reported_ = false;  // one report per boot (D7 edge rule)
};

}  // namespace bw::hal

#endif  // ARDUINO
