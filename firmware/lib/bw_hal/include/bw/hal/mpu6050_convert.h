#pragma once

// bw/hal/mpu6050_convert.h — raw MPU6050 registers -> physical units (pure;
// F01 §9 host tests: raw -> g/dps conversion from fixtures).
// Register I/O lives in the platform driver; calibration/offset estimation is
// out of scope here (F01 §12; F02 if needed) — this is conversion only (§5).

#include <cstdint>

#include <bw/hardware.h>
#include <bw/types.h>

namespace bw::hal {

// [HW] Multi-byte register values are big-endian (MSB at the lowest address,
// MPU6050 datasheet §4 register map, e.g. ACCEL_XOUT_H..L at 0x3B..0x3C).
int16_t be16(const uint8_t* p);

// [HW] Full-scale range selection bits (MPU6050 datasheet, ACCEL_CONFIG
// 0x1C / GYRO_CONFIG 0x1B — AFS_SEL / FS_SEL). The configured start values
// (+/-4 g, +/-500 dps) are F01 §8 kImuAccelRangeG / kImuGyroRangeDps.
enum class AccelRange : uint8_t { G2, G4, G8, G16 };
enum class GyroRange : uint8_t { Dps250, Dps500, Dps1000, Dps2000 };

// [HW] Sensitivity in LSB per g / LSB per (deg/s) at each full-scale range
// (MPU6050 datasheet, "Accelerometer Specifications" / "Gyroscope
// Specifications" sensitivity tables): 16384/8192/4096/2048 LSB/g and
// 131/65.5/32.8/16.4 LSB/(deg/s).
float accelLsbPerG(AccelRange range);
float gyroLsbPerDps(GyroRange range);

// One burst read's raw counts (X, Y, Z) after byte-order conversion.
struct ImuRaw {
    int16_t accel[3];
    int16_t gyro[3];
};

// Pure conversion: raw counts -> sensor-frame sample (no filtering, no offsets).
bw::ImuSample convertImuRaw(const ImuRaw& raw, bw::TimeMs t_ms, AccelRange accel_range,
                            GyroRange gyro_range);

}  // namespace bw::hal
