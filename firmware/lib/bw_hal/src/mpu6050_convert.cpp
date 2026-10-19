#include <bw/hal/mpu6050_convert.h>

namespace bw::hal {

int16_t be16(const uint8_t* p) {
    const uint16_t raw = (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
    return static_cast<int16_t>(raw);
}

float accelLsbPerG(AccelRange range) {
    switch (range) {
    case AccelRange::G2:
        return 16384.0f;
    case AccelRange::G4:
        return 8192.0f;
    case AccelRange::G8:
        return 4096.0f;
    case AccelRange::G16:
        return 2048.0f;
    }
    return 0.0f;  // unreachable for a valid enum; "no data" beats a guess
}

float gyroLsbPerDps(GyroRange range) {
    switch (range) {
    case GyroRange::Dps250:
        return 131.0f;
    case GyroRange::Dps500:
        return 65.5f;
    case GyroRange::Dps1000:
        return 32.8f;
    case GyroRange::Dps2000:
        return 16.4f;
    }
    return 0.0f;
}

bw::ImuSample convertImuRaw(const ImuRaw& raw, bw::TimeMs t_ms, AccelRange accel_range,
                            GyroRange gyro_range) {
    const float lsb_per_g = accelLsbPerG(accel_range);
    const float lsb_per_dps = gyroLsbPerDps(gyro_range);

    bw::ImuSample sample{};
    sample.t_ms = t_ms;
    // Unreachable for a valid enum value (exhaustive switch above); guards a
    // cast of garbage input from producing inf/NaN that would look like data.
    if (lsb_per_g <= 0.0f || lsb_per_dps <= 0.0f) {
        return sample;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        sample.accel_g[axis] = static_cast<float>(raw.accel[axis]) / lsb_per_g;
        sample.gyro_dps[axis] = static_cast<float>(raw.gyro[axis]) / lsb_per_dps;
    }
    return sample;
}

}  // namespace bw::hal
