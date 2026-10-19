// MPU6050 raw -> physical conversion tests (F01 §9: "MPU6050 raw -> g/dps
// conversion from fixtures"). Table-driven over all datasheet ranges.

#include <unity.h>

#include <bw/hal/mpu6050_convert.h>

using bw::hal::accelLsbPerG;
using bw::hal::AccelRange;
using bw::hal::be16;
using bw::hal::convertImuRaw;
using bw::hal::gyroLsbPerDps;
using bw::hal::GyroRange;
using bw::hal::ImuRaw;

namespace {
constexpr float kFloatTolerance = 0.001f;
}

void test_be16_reads_msb_first(void) {
    const uint8_t msb_first[] = {0x12, 0x34};
    TEST_ASSERT_EQUAL_INT16(0x1234, be16(msb_first));
}

void test_be16_sign_extends_negative_values(void) {
    const uint8_t minus_one[] = {0xFF, 0xFF};
    const uint8_t min_int16[] = {0x80, 0x00};
    TEST_ASSERT_EQUAL_INT16(-1, be16(minus_one));
    TEST_ASSERT_EQUAL_INT16(-32768, be16(min_int16));
}

void test_accel_lsb_per_g_matches_datasheet_table(void) {
    // [HW] MPU6050 sensitivity: 2/4/8/16 g full scale -> 16384/8192/4096/2048.
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 16384.0f, accelLsbPerG(AccelRange::G2));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 8192.0f, accelLsbPerG(AccelRange::G4));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 4096.0f, accelLsbPerG(AccelRange::G8));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 2048.0f, accelLsbPerG(AccelRange::G16));
}

void test_gyro_lsb_per_dps_matches_datasheet_table(void) {
    // [HW] MPU6050 sensitivity: 250/500/1000/2000 dps -> 131/65.5/32.8/16.4.
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 131.0f, gyroLsbPerDps(GyroRange::Dps250));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 65.5f, gyroLsbPerDps(GyroRange::Dps500));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 32.8f, gyroLsbPerDps(GyroRange::Dps1000));
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 16.4f, gyroLsbPerDps(GyroRange::Dps2000));
}

void test_convert_zero_raw_is_zero_and_keeps_timestamp(void) {
    const ImuRaw raw = {};
    const bw::ImuSample s = convertImuRaw(raw, 4242, AccelRange::G4, GyroRange::Dps500);

    TEST_ASSERT_EQUAL_UINT64(4242, s.t_ms);
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.0f, s.accel_g[i]);
        TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.0f, s.gyro_dps[i]);
    }
}

void test_convert_configured_range_one_g(void) {
    // F01 §8 start configuration: +/-4 g, +/-500 dps.
    ImuRaw raw = {};
    raw.accel[0] = 8192;  // 1 g on X
    raw.accel[1] = -8192;
    raw.gyro[2] = 655;  // 655 / 65.5 = 10.0 dps on Z
    const bw::ImuSample s = convertImuRaw(raw, 1, AccelRange::G4, GyroRange::Dps500);

    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f, s.accel_g[0]);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, -1.0f, s.accel_g[1]);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.0f, s.accel_g[2]);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 10.0f, s.gyro_dps[2]);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 0.0f, s.gyro_dps[0]);
}

void test_convert_covers_every_range_extremes(void) {
    struct Row {
        AccelRange ar;
        int16_t accel_counts;  // always 1 g
        GyroRange gr;
        int16_t gyro_counts;  // always 10 dps
    };
    const Row table[] = {
        {AccelRange::G2, 16384, GyroRange::Dps250, 1310},
        {AccelRange::G4, 8192, GyroRange::Dps500, 655},
        {AccelRange::G8, 4096, GyroRange::Dps1000, 328},
        {AccelRange::G16, 2048, GyroRange::Dps2000, 164},
    };
    for (const Row& row : table) {
        ImuRaw raw = {};
        raw.accel[2] = row.accel_counts;
        raw.gyro[1] = row.gyro_counts;
        const bw::ImuSample s = convertImuRaw(raw, 0, row.ar, row.gr);
        TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f, s.accel_g[2]);
        TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 10.0f, s.gyro_dps[1]);
    }
}

void test_convert_int16_extremes_do_not_overflow(void) {
    ImuRaw raw = {};
    raw.accel[0] = 32767;
    raw.gyro[0] = -32768;
    const bw::ImuSample s = convertImuRaw(raw, 0, AccelRange::G4, GyroRange::Dps500);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 32767.0f / 8192.0f, s.accel_g[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -32768.0f / 65.5f, s.gyro_dps[0]);
}

void test_convert_full_burst_fixture_pipeline(void) {
    // One burst read as the driver sees it: big-endian register bytes
    // (ACCEL_XOUT_H..L at 0x3B, then Y, Z, then GYRO at 0x43).
    const uint8_t burst[] = {
        0x20, 0x00,  // accel X = 8192  -> 1.0 g at +/-4 g
        0x00, 0x00,  // accel Y = 0
        0xE0, 0x00,  // accel Z = -8192 -> -1.0 g
        0x00, 0x00,  // gyro X = 0
        0x00, 0x00,  // gyro Y = 0
        0x02, 0x8F,  // gyro Z = 655    -> 10.0 dps at +/-500 dps
    };
    ImuRaw raw = {};
    for (int i = 0; i < 3; ++i) {
        raw.accel[i] = be16(burst + i * 2);
        raw.gyro[i] = be16(burst + 6 + i * 2);
    }
    const bw::ImuSample s = convertImuRaw(raw, 77, AccelRange::G4, GyroRange::Dps500);

    TEST_ASSERT_EQUAL_UINT64(77, s.t_ms);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 1.0f, s.accel_g[0]);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, -1.0f, s.accel_g[2]);
    TEST_ASSERT_FLOAT_WITHIN(kFloatTolerance, 10.0f, s.gyro_dps[2]);
}
