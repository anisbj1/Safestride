#include "wire_hal.h"
#include "task_health.h"

#if defined(ARDUINO)

#include <Wire.h>  // the TwoWire instance injected into Esp32I2cBackend

#include <cstdio>  // snprintf for the bench pin-finder messages

#include <bw/fault.h>

#include <config/hw_pins.h>

#include "../app.h"

#include "wire_diag.h"         // W6 P4: F23 quality mailbox feed (no direct F01 status reads)
#include "wire_reliability.h"  // W6 P4: F26 reliability mailbox feed
#include "wire_research.h"     // F28 A1 tee tap (W1 integration 2026-10-11)

#include <bw/hal/adc_battery_monitor.h>
#include <bw/hal/battery_convert.h>
#include <bw/hal/buzzer.h>
#include <bw/hal/device_clock.h>
#include <bw/hal/esp32_i2c_backend.h>
#include <bw/hal/gpio_buttons.h>
#include <bw/hal/i2c_bus.h>
#include <bw/hal/ledc_rgb_led.h>
#include <bw/hal/max30102_ppg.h>
#include <bw/hal/mpu6050_imu.h>
#include <bw/hal/u8g2_canvas.h>

namespace f01 {

namespace {
// Static-in-storage HAL graph (no heap: everything here has static storage
// duration, 00 §7.2). Declaration order is dependency order — each object
// references only objects declared above it, and all initializers are
// constant (no hardware side effects; hardware starts in wire()).

// Fault routing shim. The graph below is constructed at static-init time,
// before app::run() can hand over AppContext.faults (the field F01's status
// log leaves open for the integrator until F26 lands), so every driver points
// at this router and wire() installs the real sink just before bring-up —
// boot-time reports (ImuAbsent / PpgAbsent / DisplayFailed /
// BatterySenseUnavailable) are therefore not lost. Forwarding only: drivers
// still just call report(); F26's registry becomes the sink later, unchanged.
class FaultRouter final : public bw::IFaultReporter {
public:
    void report(bw::FaultCode code, bw::FaultSeverity severity, bw::TimeMs t_ms) override {
        if (sink_ != nullptr) {
            sink_->report(code, severity, t_ms);
        }
    }
    void setSink(bw::IFaultReporter* sink) { sink_ = sink; }

private:
    bw::IFaultReporter* sink_ = nullptr;
};

FaultRouter s_faults;
bw::hal::DeviceClock s_clock;  // D9/DEC-018: monotonic ms since boot, unix = 0
bw::hal::Esp32I2cBackend s_backend{Wire, HW_PIN_I2C_SDA, HW_PIN_I2C_SCL};
bw::hal::I2cBus s_bus{s_backend, &s_clock, &s_faults};
bw::hal::Mpu6050Imu s_imu{s_bus, &s_clock, &s_faults};
bw::hal::Max30102Ppg s_ppg{s_bus, &s_clock, &s_faults};
bw::hal::GpioButtons s_buttons{HW_PIN_BUTTON_BIG, HW_PIN_BUTTON_SMALL, HW_BUTTON_ACTIVE_LOW != 0};
bw::hal::LedcRgbLed s_led{HW_PIN_LED_R, HW_PIN_LED_G, HW_PIN_LED_B, HW_LED_PWM_CHANNEL_BASE,
                          HW_RGB_ACTIVE_LOW != 0};
bw::hal::Buzzer s_buzzer{HW_PIN_BUZZER, HW_BUZZER_IS_PASSIVE != 0, HW_BUZZER_ACTIVE_LOW != 0,
                         HW_BUZZER_PWM_CHANNEL};
bw::hal::U8g2Canvas s_display{s_bus, HW_DISPLAY_ADDR7, &s_faults, &s_clock};
bw::hal::BatteryConfig s_battery_cfg{
    HW_BATTERY_DIVIDER_PRESENT != 0,  // divider_present
    HW_BATTERY_CHARGING_KNOWN != 0,   // charging_known
    HW_BATTERY_ADC_FULL_SCALE,        // adc_full_scale (uint16 below)
    HW_BATTERY_VREF_V,                // vref_v
    HW_BATTERY_DIVIDER_RATIO,         // divider_ratio
    HW_BATTERY_CALIBRATION,           // calibration
    HW_BATTERY_V_EMPTY_V,             // v_empty_v
    HW_BATTERY_V_FULL_V,              // v_full_v
};
bw::hal::AdcBatteryMonitor s_battery{HW_PIN_BATTERY_ADC, s_battery_cfg, &s_faults, &s_clock};

// Bench boot probes (D16 accel-anomaly triage): one WHO_AM_I read (0x75,
// the same writeRead begin() proved answering at boot) after each bring-up
// stage, recorded so imuDiag() can print "which stage turned the chip from
// 0x68 into garbage" on one line. Read-only — no tracker/fault side effects
// (D16), static storage only (00 §7.2), fixed order = label order.
constexpr const char* kBootProbeLabels[] = {"imu", "ppg", "io", "disp", "batt"};
struct BootProbe {
    int err;         // bw::Err as int: 0 = Ok (avoids %d on enum class)
    uint8_t whoami;  // byte read; 0xFF when the probe itself failed
};
constexpr size_t kBootProbeStages = sizeof(kBootProbeLabels) / sizeof(kBootProbeLabels[0]);
BootProbe s_boot_probes[kBootProbeStages];
size_t s_boot_probe_n = 0;

void recordBootProbe() {
    if (s_boot_probe_n >= kBootProbeStages) {
        return;
    }
    uint8_t reg = 0x75;  // [HW] WHO_AM_I (register map; mirrors the driver)
    uint8_t id = 0xFF;
    const bw::Result<void> r = s_bus.writeRead(bw::hal::kMpu6050Addr7, &reg, 1, &id, 1);
    BootProbe p{static_cast<int>(r.err), 0xFF};
    if (r.ok()) {
        p.whoami = id;
    }
    s_boot_probes[s_boot_probe_n++] = p;
}
}  // namespace

void sensorTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Absolute-deadline pacing (runtime.md §3.2 rule: no delay(), use
    // vTaskDelayUntil): a tick lost to the display's I²C mutex hold compresses
    // the NEXT gap instead of shifting every following sample, so the 50 Hz
    // cadence self-heals after contention.
    TickType_t last_wake = xTaskGetTickCount();

    // Failed-sensor retry pacing (kSensorAbsentRetryMs): both timestamps start
    // one period in the past, so the first iteration still attempts a read
    // (unsigned wrap-around makes the first attempt due immediately) and the
    // next attempt lands one period after each try.
    const TickType_t retry_period = pdMS_TO_TICKS(kSensorAbsentRetryMs);
    TickType_t last_imu_attempt = last_wake - retry_period;
    TickType_t last_ppg_attempt = last_wake - retry_period;

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(last_wake);
        const TickType_t now_tick = xTaskGetTickCount();

        // One burst read = one fresh sample at the 50 Hz ODR (the driver
        // stamps t_ms from ctx.clock at read time). While Ok the read runs
        // every tick; while Failed only once per kSensorAbsentRetryMs — the
        // retry keeps replug recovery (readOk) working without paying the
        // failing I²C transaction + error log at 50 Hz (see wire_hal.h).
        if (s_imu.status() == bw::SensorStatus::Ok || now_tick - last_imu_attempt >= retry_period) {
            last_imu_attempt = now_tick;
            bw::ImuSample imu{};
            const bw::Result<size_t> imu_read = s_imu.read(&imu, 1);
            if (imu_read.ok() && imu_read.value > 0) {
                // Full queue: push refused and counted (CR-004) — the sensor
                // task never blocks on a consumer being late.
                (void)ctx.imu_samples.push(imu);
                // W6 P4: F23 raw quality tee (per-sample).
                f23::onImuSample(imu);
                // F28 A1 tee (W1 integration, 2026-10-11): non-consuming copy
                // tap into the F28-owned queue. Disabled (the default) it
                // costs one branch and stores nothing; full is counted by the
                // tee's own drops(), never by this task's pacing.
                if (bw::research::RawTee* tee = ::f28::rawTee()) {
                    (void)tee->onImu(imu);
                }
            }
        }

        // PPG FIFO drain (same Ok/Failed rule; well inside
        // kPpgFifoPollMaxIntervalMs while draining at full cadence). With the
        // sensor absent (PpgAbsent at boot, the A-05 history) beginAbsent()
        // leaves it Failed, so the drain is attempted only every
        // kSensorAbsentRetryMs and the pipeline simply sees no PPG samples —
        // the downstream window closes as Insufficient (tested). With the
        // MAX30102 present (2026-10-13) status stays Ok — the boot-time soft
        // reset + episode rule (F01 D11 amendment) make sure a stale FIFO or
        // a single overrun cannot latch it — and the drain runs every tick.
        if (s_ppg.status() == bw::SensorStatus::Ok || now_tick - last_ppg_attempt >= retry_period) {
            last_ppg_attempt = now_tick;
            bw::PpgSample ppg[kSensorPpgBurstSamples];
            const bw::Result<size_t> ppg_read = s_ppg.read(ppg, kSensorPpgBurstSamples);
            if (ppg_read.ok()) {
                for (size_t i = 0; i < ppg_read.value; ++i) {
                    (void)ctx.ppg_samples.push(ppg[i]);
                    // W6 P4: F23 raw quality tee (per-sample).
                    f23::onPpgSample(ppg[i]);
                    // F28 A1 tee, PPG side (see the IMU tap above).
                    if (bw::research::RawTee* tee = ::f28::rawTee()) {
                        (void)tee->onPpg(ppg[i]);
                    }
                }
            }
        }

        // W6 P4: publish sensor statuses to F23/F26 via their mailbox seams
        // (neither feature may read mutable F01 status fields directly).
        f23::noteSensorStatuses(s_imu.status(), s_ppg.status());
        f26::noteSensorStatuses(s_imu.status(), s_ppg.status());

        app::tasks::completeCycle(bw::diag::TaskSlot::Sensor, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSensorTaskPeriodMs));
    }
}

void wire(app::AppContext& ctx) {
    // F00 §13 / DEC-018: F01 supplies the device monotonic clock before M1
    // (app.h names F01 or the integrator; F01 takes it — D9).
    ctx.clock = &s_clock;

    // Install the composition root's fault sink BEFORE bring-up: the boot-time
    // absence reports (ImuAbsent/PpgAbsent/DisplayFailed/BatterySenseUnavailable)
    // are exactly the ones the bench needs to see. `ctx.faults` is nullptr
    // until the integrator assigns one — the router drops reports then, which
    // is the F01 D7 fallback the drivers were built around.
    s_faults.setSink(ctx.faults);

    // Bus first: every begin() below goes through it. If this fails, lock()
    // fails safe (Err::Busy), the probes report their device as absent and
    // boot continues (spec §10) — nothing here may block or crash.
    (void)s_backend.begin();

    // Device bring-up. Drivers record failures in status() and report through
    // IFaultReporter themselves; wiring cannot act on the results (Serial is
    // off-limits outside the composition root — 01 §1) and spec §10 makes
    // "absent at boot" a normal, survivable case.
    (void)s_imu.begin();
    recordBootProbe();  // baseline: the driver just proved 0x68 answers
    (void)s_ppg.begin();
    recordBootProbe();
    s_buttons.begin();
    // A-08 bench wiring: the RGB common anode is fed from GPIO25, not 3V3 —
    // source it before LEDC starts driving the colour pins, otherwise the
    // active-low cathode drives sink into a floating anode (dark LED).
    pinMode(HW_PIN_LED_COMMON, OUTPUT);
    digitalWrite(HW_PIN_LED_COMMON, HIGH);
    s_led.begin();
    s_buzzer.begin();
    recordBootProbe();  // GPIO/LEDC block (buttons, RGB, buzzer)
    (void)s_display.begin();
    recordBootProbe();
    s_battery.begin();
    recordBootProbe();

    // SensorTask creation — the D15 task-table step (2026-10-08). It starts
    // only AFTER every bring-up above, so the first read never races a probe,
    // and AppContext's D10 queues already exist (ctx is fully constructed
    // before the wire ladder runs). The task control block and stack are
    // allocated here, at construction time — no heap after startup (00 §7.2).
    // Creation failure leaves the queues unpopulated: downstream windows then
    // close as Insufficient (a tested state), never a crash.
    app::tasks::checkCreated(xTaskCreatePinnedToCore(sensorTask, "SensorTask",
                                                     kSensorTaskStackBytes, &ctx,
                                                     kSensorTaskPriority, nullptr, kSensorTaskCore),
                             bw::diag::TaskSlot::Sensor, ctx.faults);
}

bw::IImuSensor& imu() {
    return s_imu;
}
bw::IPpgSensor& ppg() {
    return s_ppg;
}
bw::IButtonInput& buttons() {
    return s_buttons;
}
bw::IRgbLed& led() {
    return s_led;
}
bw::IBuzzer& buzzer() {
    return s_buzzer;
}
bw::IDisplayCanvas& display() {
    return s_display;
}
bw::IBatteryMonitor& battery() {
    return s_battery;
}

bw::Err i2cScan(uint8_t* found, size_t max_found, size_t* count_out) {
    return s_bus.scan(found, max_found, count_out);
}

bw::Err benchBusRead(uint8_t addr7, uint8_t reg, uint8_t* out) {
    uint8_t byte = 0;
    const bw::Result<void> r = s_bus.writeRead(addr7, &reg, 1, &byte, 1);
    if (r.err != bw::Err::Ok) {
        return r.err;  // `out` untouched; the caller prints errName
    }
    *out = byte;
    return bw::Err::Ok;
}

bw::Err benchRawRead(uint8_t addr7, uint8_t reg, uint8_t* out, bool stop_between) {
    return s_backend.benchRawRead(addr7, reg, out, stop_between);
}

bw::Err imuDiag(char* msg, size_t msg_len) {
    if (msg == nullptr || msg_len == 0) {
        return bw::Err::InvalidArg;
    }
    msg[0] = '\0';
    const bw::Result<bw::hal::Mpu6050Diag> diag = s_imu.readRawDiagnostics();
    if (!diag.ok()) {
        return diag.err;  // composition root prints errName(diag.err)
    }
    const bw::hal::Mpu6050Diag& v = diag.value;
    const int used =
        snprintf(msg, msg_len,
                 "raw=%d/%d/%d gyro=%d/%d/%d off=%d/%d/%d div=%u "
                 "cfg=0x%02X gcfg=0x%02X acfg=0x%02X pwr=0x%02X "
                 "id=0x%02X",
                 v.accel_counts[0], v.accel_counts[1], v.accel_counts[2], v.gyro_counts[0],
                 v.gyro_counts[1], v.gyro_counts[2], v.accel_offset[0], v.accel_offset[1],
                 v.accel_offset[2], static_cast<unsigned>(v.sample_rate_div), v.config,
                 v.gyro_config, v.accel_config, v.power_mgmt1, v.whoami);
    if (used < 0 || static_cast<size_t>(used) >= msg_len) {
        return bw::Err::Ok;  // budget bug, not a bus error: line already fits
    }
    // Boot-stage WHO_AM_I trail (D16): one bench line now carries both "what
    // the chip answers now" and "which bring-up stage first answered 0x00 /
    // failed" — bounded by kImuDiagMsgChars, so snprintf cannot truncate.
    size_t off = static_cast<size_t>(used);
    for (size_t i = 0; i < s_boot_probe_n && i < kBootProbeStages; ++i) {
        const BootProbe& p = s_boot_probes[i];
        const int n = (p.err == 0)
                          ? snprintf(msg + off, msg_len - off, "%s%s:%d/%02X",
                                     (i == 0) ? " stg=" : " ", kBootProbeLabels[i], p.err, p.whoami)
                          : snprintf(msg + off, msg_len - off, "%s%s:%d/--",
                                     (i == 0) ? " stg=" : " ", kBootProbeLabels[i], p.err);
        if (n < 0 || static_cast<size_t>(n) >= msg_len - off) {
            break;  // cannot happen within the budget; never overrun
        }
        off += static_cast<size_t>(n);
    }
    // PPG readback facts (D17, 2026-10-13 loop-2): the same bench line now
    // discriminates the silicon truths that latch-failed PPG on hardware —
    //   rst=IGN : the chip ignored the soft-reset write (stale pointers after)
    //   clr=POP : the 3-register clear write does not stick; the driver pops
    //             the FIFO instead; ovf=STK means OVF_PTR proved permanently
    //             non-zero and is ignored from then on (detection unavailable)
    //   ep=N    : bounded reinitializations run by the R2 ladder (0 = never)
    const bw::hal::Max30102PpgDiag& pd = s_ppg.diag();
    const int ppg_n = snprintf(msg + off, msg_len - off, " ppg:rst=%s clr=%s ovf=%s ep=%u",
                               pd.reset_cleared ? "OK" : "IGN",
                               pd.clear_via_write ? "W" : (pd.ovf_stuck ? "POP" : "--"),
                               pd.ovf_stuck ? "STK" : "ok", static_cast<unsigned>(pd.reinit_runs));
    if (ppg_n < 0 || static_cast<size_t>(ppg_n) >= msg_len - off) {
        return bw::Err::Ok;  // budget bug, not a bus error: bounded by construction
    }
    return bw::Err::Ok;
}

namespace {
// [HW] Candidate OUTPUT pins for A-08 (which GPIO drives the RGB LED).
// Deliberately excluded: 1/3 (UART0 — would kill the monitor), 21/22 (the
// I²C bus the panel answers on), 26/27 (button pins — driving LOW against a
// pressed-to-3.3 V button would short it), 32-39 (input-only pins), 16
// (buzzer, row ⑥ already proved it), 13/14/4 (owned by the RGB's LEDC
// channels — the normal colour cycle exercises those), and 12 (MTDI strap:
// HIGH at reset selects 1.8 V flash and would need a power cycle to undo).
// The remaining strapping pins (2/5/15) are safe while running: their level
// is sampled only at reset, and pinMode state does not survive a reset.
constexpr uint8_t kFinderDrivePins[] = {2, 5, 15, 17, 18, 19, 23, 25};

// [HW] Candidate INPUT pins for A-07 (the small button): pulled up, so a
// press-to-GND reads 1→0 and proves the pin. Excludes the driven set (our own
// pattern must never look like a press) and 16 (LEDC owns the buzzer pin).
constexpr uint8_t kFinderInputPins[] = {0, 26, 27, 32, 33, 35};

constexpr size_t kFinderDriveCount = sizeof(kFinderDrivePins);
constexpr size_t kFinderInputCount = sizeof(kFinderInputPins);
}  // namespace

bool pinFinderStep(uint32_t now_ms, char* msg, size_t msg_len) {
    // Function-local statics: one caller, bench scaffolding, static storage
    // only (00 §7.2) — and the composition root needs no extra state.
    static bool started = false;
    static uint32_t phase_started_ms = 0;
    static size_t drive_idx = 0;
    static bool driving_high = false;
    static uint8_t prev_in[kFinderInputCount] = {};

    if (!started) {
        started = true;
        phase_started_ms = now_ms;
        for (size_t i = 0; i < kFinderInputCount; ++i) {
            pinMode(kFinderInputPins[i], INPUT_PULLUP);
            prev_in[i] = digitalRead(kFinderInputPins[i]) ? 1 : 0;
        }
        pinMode(kFinderDrivePins[drive_idx], OUTPUT);
        digitalWrite(kFinderDrivePins[drive_idx], HIGH);
        driving_high = true;
        snprintf(msg, msg_len, "drive GPIO%u HIGH - lit?",
                 static_cast<unsigned>(kFinderDrivePins[drive_idx]));
        return true;
    }

    // Input edges first: a button press must never be hidden behind a phase
    // change. Pull-up wiring means 1->0 is a press-to-GND (A-07 active-low).
    for (size_t i = 0; i < kFinderInputCount; ++i) {
        const uint8_t level = digitalRead(kFinderInputPins[i]) ? 1 : 0;
        if (level != prev_in[i]) {
            snprintf(msg, msg_len,
                     "input GPIO%u %u->%u (press=1->0 means "
                     "active-low)",
                     static_cast<unsigned>(kFinderInputPins[i]), static_cast<unsigned>(prev_in[i]),
                     static_cast<unsigned>(level));
            prev_in[i] = level;
            return true;
        }
    }

    if (static_cast<uint32_t>(now_ms - phase_started_ms) < kPinFinderPhaseMs) {
        return false;
    }
    phase_started_ms = now_ms;

    const uint8_t pin = kFinderDrivePins[drive_idx];
    if (driving_high) {
        // Same pin, opposite level: an active-low (common-anode) LED lights
        // on this half of the cycle, an active-high one on the previous half.
        digitalWrite(pin, LOW);
        driving_high = false;
        snprintf(msg, msg_len, "drive GPIO%u LOW - lit? (yes = active-low)",
                 static_cast<unsigned>(pin));
        return true;
    }

    // Phase over: release this candidate, move to the next one.
    pinMode(pin, INPUT);  // hi-Z, back to as-found
    drive_idx = (drive_idx + 1) % kFinderDriveCount;
    pinMode(kFinderDrivePins[drive_idx], OUTPUT);
    digitalWrite(kFinderDrivePins[drive_idx], HIGH);
    driving_high = true;
    snprintf(msg, msg_len, "drive GPIO%u HIGH - lit?",
             static_cast<unsigned>(kFinderDrivePins[drive_idx]));
    return true;
}

}  // namespace f01

#endif  // ARDUINO
