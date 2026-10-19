#pragma once

// firmware/src/app/wiring/wire_hal.h — F01's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_hal.* per F01 spec §2
// Owns — see status log for the wire_f01.* reconciliation note).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).
// Platform-only (F01 D5): the HAL objects behind these declarations exist
// only where ARDUINO is defined, same guard as device_clock.h.

#if defined(ARDUINO)

#include <cstddef>
#include <cstdint>

#include <bw/hardware.h>

#include <config/hw_config.h>

namespace app {
struct AppContext;
}

namespace f01 {

// --- SensorTask (runtime.md §3.2: F01 owns SensorTask) --------------------
// Task table instantiated 2026-10-08 (D15): wire() creates the task after
// device bring-up; its body pushes samples into AppContext's D10 queues, which
// F02's ProcessingTask drains.

// [DERIVED] 1000 ms / HW_IMU_ODR_HZ: one IMU burst read per tick keeps the
// 50 Hz cadence; the PPG FIFO is drained inside the same task at
// <= HW_PPG_DRAIN_MS (kPpgFifoPollMaxIntervalMs).
constexpr uint32_t kSensorTaskPeriodMs = HW_SENSOR_PERIOD_MS;

// [EXPERIMENTAL] Stack bytes: measured on device 2026-10-08 — task entry
// already sits 636 B deep (probe), and the *failing* read path adds the
// task → driver → I2cBus → Wire call chain (~1.1 KB of frames) plus Wire's
// error log (newlib vprintf ≈ 0.7 KB). A 2048 B stack underflowed past its
// base during that log and smashed the I2C driver's cmd_evt_queue heap block
// (144 B below the stack) → xQueueGenericSendFromISR assert on boot. 4 KB
// keeps ≥1.5 KB margin on the deepest observed path (display frame buffer
// stays off this task).
constexpr uint32_t kSensorTaskStackBytes = 4096;

// [DERIVED] Retry period while a sensor's SensorStatus is Failed: the read is
// skipped every tick and retried once per period instead. Keeps
// sensor_status.h's replug recovery intact (a successful retry read flips
// Failed → Ok via readOk(), so a replugged device is back at full cadence
// within one period) while the failing I²C transaction and its error log drop
// from one attempt per 20 ms tick to one per 5 s (0.2 lines/s). 5 s: no spec
// bound — the smallest round value that cuts the failure path 250x while
// keeping replug detection within 5 s.
constexpr uint32_t kSensorAbsentRetryMs = 5000;

// [EXPERIMENTAL] Maps runtime.md §3.2's "high" into ESP32's 0–25 range:
// above the "med" tasks (intelligence/decision), below the supervisor; tuned
// to the instantiated task table 2026-10-08 (ProcessingTask = 4, F03 pump =
// 3 below it, so the high > med-high > med ladder holds).
constexpr uint32_t kSensorTaskPriority = 5;

// [HW/runtime] Core 1, exactly as runtime.md §3.2 assigns SensorTask
// (core 0 hosts BLE/storage/watchdog).
constexpr uint32_t kSensorTaskCore = 1;

// [DERIVED] PPG samples drained per tick: worst healthy interval between
// drains is kPpgFifoPollMaxIntervalMs (100 ms) at kPpgSampleRateHz (100 Hz)
// = 10 FIFO entries, so 16 gives +60 % margin for one late tick; stays well
// under the FIFO's own 32-entry depth, and a real overrun is reported by the
// driver (CR-004). Stack cost: 16 × 16 B = 256 B inside kSensorTaskStackBytes.
constexpr size_t kSensorPpgBurstSamples = 16;

// Constructs the HAL graph, starts the shared I²C bus, brings up every device
// and injects the device monotonic clock (D9/DEC-018) into AppContext.
// Idempotence/order contract: call once, before any feature's wire().
void wire(app::AppContext& ctx);

// SensorTask body (runtime.md §3.2, F01 D15 — declared per wiring README):
// 20 ms cadence, one IMU burst read + one PPG FIFO drain per tick while both
// sensors are Ok, each sample pushed to the AppContext D10 queues; a Failed
// sensor is retried only every kSensorAbsentRetryMs (kSensorAbsentRetryMs
// rationale — replug recovery via readOk() still applies). Read errors are
// non-events here — status()/the fault counters already carry them (same rule
// as the bench poll this replaces). Created by wire(); `arg` is the
// AppContext.
void sensorTask(void* arg);

// Composition-root accessors: concrete instances are private to wire_hal.cpp;
// the integrator passes these bw_core interface references on to features
// through their wiring — features must never include F01's concrete headers
// (01 §1: compile-time dependencies stop at bw_core).
bw::IImuSensor& imu();
bw::IPpgSensor& ppg();
bw::IButtonInput& buttons();
bw::IRgbLed& led();
bw::IBuzzer& buzzer();
bw::IDisplayCanvas& display();
bw::IBatteryMonitor& battery();

// Diagnostic bus walk (F01 spec §9 bench checklist "I²C scan"): delegates to
// I2cBus::scan, the single I²C access point (00 §3), so the composition root
// can print what answers on the bus without touching Wire itself. Returns
// Err::Ok (count in `count_out`) or the bus error that aborted the walk.
// Bench scaffolding: no feature may call this — sensors go through the drivers.
bw::Err i2cScan(uint8_t* found, size_t max_found, size_t* count_out);

// [DERIVED] Payload budget for imuDiag(): worst case is three signed-16-bit
// triples (3 x 6 chars + 2 separators = 20 each = 60, plus the "raw= gyro=
// off=" labels = 15) plus six labeled register fields (54) = 129 chars;
// plus the boot-stage probe trail (D16: " stg=" + 5 x "label:err/id" at
// up to 11 chars = 59) = 188 chars; plus the PPG readback facts (D17:
// " ppg:rst=IGN clr=POP ovf=STK ep=65535" = 37) = 225 chars; 256 leaves
// headroom so snprintf never truncates the bench line.
constexpr size_t kImuDiagMsgChars = 256;

// Bench-only register echo (F01 D16 — accel-scale anomaly triage): reads the
// IMU's raw burst, accel offsets and config registers through Mpu6050Imu
// (still only I2cBus talks to the bus) and formats one payload line into
// `msg` (NUL-terminated), including the boot-stage WHO_AM_I trail wire()
// recorded after each bring-up stage (D16: bisects when the chip stopped
// answering 0x68). On failure `msg` is emptied and the first error is
// returned — the composition root prints it with errName, because Serial
// stays out of F01 (01 §1, same rule as pinFinderStep). No effect on
// status() or fault counting (D16). Bench scaffolding: no feature may call
// this — features use the sensors through bw_core interfaces.
bw::Err imuDiag(char* msg, size_t msg_len);

// Bench-only bus-path register read (F01 D16 anomaly triage): delegates to
// I2cBus::writeRead, i.e. exactly the path every driver read takes (register
// pointer + repeated start). The [RAW] bench matrix compares this against the
// backend's raw variants to locate where data goes wrong. Returns the read
// error (NotFound = address absent); `out` is only written on Ok. Scaffolding:
// features never call this.
bw::Err benchBusRead(uint8_t addr7, uint8_t reg, uint8_t* out);

// Bench-only delegate to Esp32I2cBackend::benchRawRead (F01 D16 triage): raw
// Wire register read bypassing the repeated-start merging — see the backend
// header for the stop_between split. Kept here so composition-root scaffolding
// never reaches around the wiring layer. Scaffolding, same removal rule.
bw::Err benchRawRead(uint8_t addr7, uint8_t reg, uint8_t* out, bool stop_between);

// [HW] ID registers and promised IDs for the [RAW] bench matrix (D16 triage):
// MPU6050 WHO_AM_I (register 0x75, datasheet value 0x68) and BMP180 chip ID
// (register 0xD0, datasheet value 0x55) — the BMP180 shares the GY-87's
// power, ground and wires with the MPU6050, so reading both through three
// paths splits bus/wiring fault from chip fault from read-path fault.
constexpr uint8_t kMpuWhoAmIReg = 0x75;
constexpr uint8_t kBmp180Addr7 = 0x77;
constexpr uint8_t kBmp180ChipIdReg = 0xD0;
constexpr uint8_t kBmp180ChipIdExpected = 0x55;

// [EXPERIMENTAL] 1000 ms per phase: long enough to see which pin lights the
// LED by eye (9 pins x 2 phases = an 18 s loop); tune while at the bench.
constexpr uint32_t kPinFinderPhaseMs = 1000;

// Bench-only pin finder (Bench A rows ④/⑤ — A-07 small button, A-08 RGB):
// walks a table of candidate OUTPUT pins, driving each HIGH for
// kPinFinderPhaseMs, LOW for kPinFinderPhaseMs, then releasing it, and in
// parallel reports any edge seen on a candidate INPUT pin (pull-up, so a
// press-to-GND shows as 1→0 and an active-high wiring shows as 0→1).
// Call every tick from the composition root and print `msg` — Serial stays out
// of F01 (01 §1). Returns true when `msg` holds a new event (NUL-terminated).
bool pinFinderStep(uint32_t now_ms, char* msg, size_t msg_len);

}  // namespace f01

#endif  // ARDUINO
