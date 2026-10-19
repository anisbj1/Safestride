#pragma once

// bw/hal/mpu6050_imu.h — MPU6050 (GY-87) driver behind bw::IImuSensor (F01).
//
// Pure register protocol on top of I2cBus: no Arduino/FreeRTOS calls, so the
// full begin/read/failure behaviour runs on the host with a scripted register
// device (F01 D10, spec §9). Only I2cBus touches the bus (00 §3); this file
// only speaks registers. No filtering, no decision logic (spec §5).

#include <cstddef>
#include <cstdint>

#include <bw/clock.h>
#include <bw/fault.h>
#include <bw/hardware.h>
#include <bw/result.h>
#include <bw/types.h>

#include <bw/hal/i2c_bus.h>
#include <bw/hal/mpu6050_convert.h>
#include <bw/hal/sensor_status.h>

namespace bw::hal {

// [HW] 7-bit I²C address (MPU6050 datasheet register map; AD0 tied low on the
// GY-87 module).
constexpr uint8_t kMpu6050Addr7 = 0x68;

// [HW] WHO_AM_I (register 0x75) reads 0x68 for an MPU6050. A-05 keeps clone
// IDs an open question: a mismatch is reported (ImuAbsent), never ignored.
constexpr uint8_t kExpectedMpuWhoAmI = 0x68;

// [DERIVED] Start rate (F01 §8): wrist motion content is mostly below 20 Hz,
// so 50 Hz satisfies Nyquist with margin at one small burst read per cycle.
constexpr uint16_t kImuSampleRateHz = 50;

// [DERIVED] DLPF_CFG = 3: 44 Hz accel / 42 Hz gyro bandwidth (MPU6050
// Register Map, CONFIG/DLPF table). Why not 1 (the previous value): our
// 50 Hz ODR has Nyquist at 25 Hz, and 184/188 Hz corners barely attenuate
// before the on-chip decimation, so 25–184 Hz vibration aliases into the
// band; 44/42 Hz is the anti-alias the decimation needs, and it is the
// setting the developer's reference project (gy87, validated 2026-10-08:
// smooth accurate tracking on this exact module) runs. Why not the reset
// default 0: with the DLPF disabled the gyro output rate is 8 kHz, so
// SMPLRT_DIV below would need 8000/50-1. Every DLPF_CFG in 1..6 gives the
// 1 kHz base the divider math assumes; this is hardware-level anti-alias
// only — F02 owns real filtering (§5).
constexpr uint8_t kImuDlpfCfg = 3;

// [DERIVED] SMPLRT_DIV for kImuSampleRateHz with the DLPF enabled:
// sample rate = 1 kHz / (1 + SMPLRT_DIV) → 1000/50 - 1 = 19.
constexpr uint8_t kImuSampleRateDiv = static_cast<uint8_t>(1000u / kImuSampleRateHz - 1u);

// [EXPERIMENTAL] Start ranges (F01 §8): ±4 g / ±500 dps cover walking/running
// arm swing without saturating while keeping fine resolution; tune with
// recorded data (F28/F29). The register values are the AFS_SEL/FS_SEL = 01
// encodings (datasheet ACCEL_CONFIG 0x1C / GYRO_CONFIG 0x1B: range bits are
// [4:3]) — computed from the same enums the converter uses so configuration
// and conversion can never disagree.
constexpr uint8_t kImuAccelConfigValue =
    static_cast<uint8_t>(static_cast<uint8_t>(AccelRange::G4) << 3);
constexpr uint8_t kImuGyroConfigValue =
    static_cast<uint8_t>(static_cast<uint8_t>(GyroRange::Dps500) << 3);

// Range actually configured by begin() — the converter input in read().
constexpr AccelRange kImuAccelRange = AccelRange::G4;
constexpr GyroRange kImuGyroRange = GyroRange::Dps500;

// Bench diagnostics snapshot (F01 D16): raw 16-bit register values exactly as
// the silicon returns them, plus the configuration registers echoed back. It
// exists to triage "accel reads 4 g at rest" on the bench: raw counts show
// whether the scale factor is non-MPU without trusting our own conversion,
// the config readbacks prove whether begin()'s writes took, and the accel
// offset registers test the offset hypothesis. Interpretation is offline
// bench analysis, not driver logic (spec §5).
struct Mpu6050Diag {
    int16_t accel_counts[3];  // ACCEL_X/Y/ZOUT_H:L, sensor frame, no scaling
    int16_t gyro_counts[3];   // GYRO_X/Y/ZOUT_H:L, sensor frame, no scaling
    int16_t accel_offset[3];  // XA/YA/ZA_OFFS_H:L raw register units ([HW]
                              // 0x06..0x0B; LSB meaning is the datasheet's,
                              // read for the offset hypothesis only)
    uint8_t sample_rate_div;  // 0x19 echo — begin() writes kImuSampleRateDiv
    uint8_t config;           // 0x1A echo — begin() writes kImuDlpfCfg
    uint8_t gyro_config;      // 0x1B echo — begin() writes kImuGyroConfigValue
    uint8_t accel_config;     // 0x1C echo — begin() writes kImuAccelConfigValue
    uint8_t power_mgmt1;      // 0x6B echo — begin() writes kPowerMgmt1Wake
    uint8_t whoami;           // 0x75 echo — kExpectedMpuWhoAmI
};

class Mpu6050Imu final : public bw::IImuSensor {
public:
    // `clock` and `faults` may be nullptr (F01 D7/D9 fallbacks): samples are
    // stamped 0 and fault reports are dropped; all other behaviour is the same.
    Mpu6050Imu(I2cBus& bus, const bw::IClock* clock, bw::IFaultReporter* faults)
        : bus_(bus), clock_(clock), faults_(faults) {}

    // WHO_AM_I probe + wake/config writes. Err::NotFound (NACK or ID mismatch)
    // marks the device Failed and reports ImuAbsent once per absent episode
    // (spec §10: boot continues on begin() failure).
    bw::Result<void> begin() override;

    // One burst read -> one ImuSample (the 50 Hz SensorTask cycle reads once).
    // max_samples == 0 returns Ok/0 without touching the bus (02 §3).
    bw::Result<size_t> read(bw::ImuSample* out, size_t max_samples) override;

    bw::SensorStatus status() const override { return tracker_.status(); }

    // Re-run begin(); when to call it belongs to F26 (02 §3).
    bw::Result<void> reinit() override { return begin(); }

    // Bench-only register echo (F01 D16 — accel-scale anomaly triage):
    // five reads (data burst, accel offsets, config block, PWR_MGMT_1,
    // WHO_AM_I) with NO side effects: status(), the failure streak (D8) and
    // fault reports are deliberately untouched, so probing a dying sensor
    // can never skew the counters it is diagnosing. Bus-level faults are
    // still reported by I2cBus itself (D7 edge rule, unchanged). Returns the
    // first transfer error (e.g. NotFound while the chip is unplugged).
    bw::Result<Mpu6050Diag> readRawDiagnostics();

private:
    void reportEdge(bw::FaultCode code, bw::FaultSeverity severity);

    I2cBus& bus_;
    const bw::IClock* clock_;
    bw::IFaultReporter* faults_;
    SensorStatusTracker tracker_{kFailuresBeforeFailed};
    bool absent_reported_ = false;  // one ImuAbsent per absent episode (D7 rule)
};

}  // namespace bw::hal
