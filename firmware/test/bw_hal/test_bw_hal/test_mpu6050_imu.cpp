// Host tests for the Mpu6050Imu driver (F01 §9: register protocol runs
// against a scripted register device — F01 D10).

#include <cstring>

#include <unity.h>

#include <bw/testkit/fake_clock.h>
#include <bw/testkit/recording_fault_reporter.h>

#include <bw/hal/i2c_bus.h>
#include <bw/hal/mpu6050_imu.h>

#include "fake_register_device.h"

using bw::FaultCode;
using bw::FaultSeverity;
using bw::SensorStatus;
using bw::hal::I2cBus;
using bw::hal::kMpu6050Addr7;
using bw::hal::Mpu6050Imu;
using bw::hal::test::FakeRegisterBus;
using bw::hal::test::FakeRegisterDevice;
using bw::testkit::FakeClock;
using bw::testkit::RecordingFaultReporter;

namespace {

constexpr uint8_t kRegAccelXoutH = 0x3B;  // [HW] datasheet register map

// Big-endian helper for seeding burst fixtures.
void putBe16(uint8_t* p, int16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

struct Fixture {
    FakeRegisterBus bus;
    FakeClock clock;
    RecordingFaultReporter<8> faults;
    I2cBus i2c;
    Mpu6050Imu imu;

    Fixture() : i2c(bus, &clock, &faults), imu(i2c, &clock, &faults) {
        // Device powers up with WHO_AM_I = 0x68 (datasheet default).
        bus.device(kMpu6050Addr7).regs[0x75] = 0x68;
    }
    FakeRegisterDevice& dev() { return bus.device(kMpu6050Addr7); }
};

}  // namespace

void test_mpu_begin_with_valid_id_configures_and_marks_ok(void) {
    Fixture f;
    bw::Result<void> r = f.imu.begin();
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.imu.status()));
    // Wake + range/rate configuration landed in the register file.
    TEST_ASSERT_EQUAL_HEX8(0x01, f.dev().regs[0x6B]);  // PWR_MGMT_1: PLL, awake
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuSampleRateDiv, f.dev().regs[0x19]);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuDlpfCfg, f.dev().regs[0x1A]);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuGyroConfigValue, f.dev().regs[0x1B]);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuAccelConfigValue, f.dev().regs[0x1C]);
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
}

void test_mpu_begin_wrong_whoami_reports_absent_once(void) {
    Fixture f;
    f.dev().regs[0x75] = 0x69;  // not an MPU6050 (A-05 clone scenario)
    bw::Result<void> r = f.imu.begin();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotFound), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::ImuAbsent));
    // D7 edge rule: a second begin in the same absent episode stays silent.
    r = f.imu.begin();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::ImuAbsent));
    // A successful begin (replug + reinit) re-arms the latch.
    f.dev().regs[0x75] = 0x68;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    f.dev().regs[0x75] = 0x69;
    TEST_ASSERT_FALSE(f.imu.begin().ok());
    TEST_ASSERT_EQUAL_UINT(2, f.faults.countOf(FaultCode::ImuAbsent));
}

void test_mpu_begin_nack_reports_absent(void) {
    Fixture f;
    f.dev().nack = true;
    bw::Result<void> r = f.imu.begin();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotFound), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::ImuAbsent));
}

void test_mpu_begin_bus_error_propagates_and_stays_uninitialized(void) {
    Fixture f;
    f.dev().err = bw::Err::Timeout;  // bus outage, not an absent chip
    bw::Result<void> r = f.imu.begin();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Timeout), static_cast<int>(r.err));
    // Not "absent": the bus faults were reported by I2cBus (D7), the driver
    // must not double-report, and reinit() stays available (spec §10).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Uninitialized),
                          static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::ImuAbsent));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::ImuReadFailed));
}

void test_mpu_read_returns_sample_with_timestamp(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    f.clock.setMs(1234);

    // Fixture: +1.0 g on X (8192 LSB/g at ±4 g), +1.0 dps on X
    // (65.5 LSB/(deg/s) at ±500 dps), rest zero.
    uint8_t burst[14] = {};
    putBe16(burst + 0, 8192);
    putBe16(burst + 8, 65);  // truncated 65.5 -> assert with tolerance below
    std::memcpy(f.dev().regs + kRegAccelXoutH, burst, sizeof(burst));

    bw::ImuSample sample{};
    bw::Result<size_t> r = f.imu.read(&sample, 1);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(1, r.value);
    TEST_ASSERT_EQUAL_UINT64(1234, sample.t_ms);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, sample.accel_g[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, sample.accel_g[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.0f, sample.gyro_dps[0]);  // 65/65.5
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
}

void test_mpu_five_consecutive_failures_report_read_failed_once(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    f.dev().nack = true;  // chip unplugged after boot

    bw::ImuSample sample{};
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.imu.status()));
        TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::ImuReadFailed));
    }
    // 5th consecutive failure = the D8 transition edge.
    TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::ImuReadFailed));
    // Already Failed: further failures stay silent (one report per episode).
    TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::ImuReadFailed));
}

void test_mpu_success_between_failures_resets_streak(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    bw::ImuSample sample{};

    for (int i = 0; i < 4; ++i) {
        f.dev().nack = true;
        TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
        f.dev().nack = false;
        TEST_ASSERT_TRUE(f.imu.read(&sample, 1).ok());
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::ImuReadFailed));
}

void test_mpu_read_after_replug_recovers_without_reinit(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    bw::ImuSample sample{};
    f.dev().nack = true;
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.imu.status()));
    // Replugged: the device answers again, tracker.readOk covers it (D8) —
    // F26 may still call reinit(), but nothing is stuck without it.
    f.dev().nack = false;
    TEST_ASSERT_TRUE(f.imu.read(&sample, 1).ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.imu.status()));
}

void test_mpu_read_zero_max_samples_does_not_touch_the_bus(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    const size_t before = f.bus.transferCount();
    bw::Result<size_t> r = f.imu.read(nullptr, 0);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(0, r.value);
    TEST_ASSERT_EQUAL_UINT(before, f.bus.transferCount());
}

void test_mpu_diag_returns_raw_counts_and_register_readbacks(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());

    // Seed an anomaly-shaped frame (the bench's "4.02 g at rest" signature:
    // |raw| ≈ 32 768 counts) plus known accel offsets (D16 hypotheses).
    uint8_t burst[14] = {};
    putBe16(burst + 0, 12861);
    putBe16(burst + 2, -30148);
    putBe16(burst + 4, 3441);
    putBe16(burst + 8, -186);  // gyro X only; Y/Z stay 0
    std::memcpy(f.dev().regs + kRegAccelXoutH, burst, sizeof(burst));
    putBe16(f.dev().regs + 0x06, 111);   // XA_OFFS_H:L
    putBe16(f.dev().regs + 0x08, -222);  // YA_OFFS_H:L
    putBe16(f.dev().regs + 0x0A, 333);   // ZA_OFFS_H:L

    bw::Result<bw::hal::Mpu6050Diag> d = f.imu.readRawDiagnostics();
    TEST_ASSERT_TRUE(d.ok());
    TEST_ASSERT_EQUAL_INT16(12861, d.value.accel_counts[0]);
    TEST_ASSERT_EQUAL_INT16(-30148, d.value.accel_counts[1]);
    TEST_ASSERT_EQUAL_INT16(3441, d.value.accel_counts[2]);
    TEST_ASSERT_EQUAL_INT16(-186, d.value.gyro_counts[0]);
    TEST_ASSERT_EQUAL_INT16(0, d.value.gyro_counts[1]);
    TEST_ASSERT_EQUAL_INT16(0, d.value.gyro_counts[2]);
    TEST_ASSERT_EQUAL_INT16(111, d.value.accel_offset[0]);
    TEST_ASSERT_EQUAL_INT16(-222, d.value.accel_offset[1]);
    TEST_ASSERT_EQUAL_INT16(333, d.value.accel_offset[2]);
    // Config echo == what begin() wrote: this is the "did the write take?"
    // evidence for the bench (D12/D16).
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuSampleRateDiv, d.value.sample_rate_div);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuDlpfCfg, d.value.config);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuGyroConfigValue, d.value.gyro_config);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kImuAccelConfigValue, d.value.accel_config);
    TEST_ASSERT_EQUAL_HEX8(0x01, d.value.power_mgmt1);  // kPowerMgmt1Wake
    TEST_ASSERT_EQUAL_HEX8(0x68, d.value.whoami);
    // The probe is invisible to status and faults (D16).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
}

void test_mpu_diag_failure_leaves_streak_and_faults_untouched(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    bw::ImuSample sample{};
    f.dev().nack = true;
    for (int i = 0; i < 4; ++i) {  // streak sits one below kFailuresBeforeFailed
        TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
    }
    // A failing probe must NOT count as the 5th failure...
    bw::Result<bw::hal::Mpu6050Diag> d = f.imu.readRawDiagnostics();
    TEST_ASSERT_FALSE(d.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotFound), static_cast<int>(d.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::ImuReadFailed));
    // ...so the next real read failure is exactly the transition edge (5).
    TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::ImuReadFailed));
}

void test_mpu_diag_success_does_not_reset_failure_streak(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.imu.begin().ok());
    bw::ImuSample sample{};
    f.dev().nack = true;
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
    }
    f.dev().nack = false;
    // A successful probe is not a sample: readOk() must not reset the streak
    // while the device still fails every real read.
    TEST_ASSERT_TRUE(f.imu.readRawDiagnostics().ok());
    f.dev().nack = true;
    TEST_ASSERT_FALSE(f.imu.read(&sample, 1).ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.imu.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::ImuReadFailed));
}

void test_mpu_diag_absent_returns_not_found_without_reporting(void) {
    Fixture f;
    f.dev().nack = true;  // chip never present: begin() was never possible
    bw::Result<bw::hal::Mpu6050Diag> d = f.imu.readRawDiagnostics();
    TEST_ASSERT_FALSE(d.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotFound), static_cast<int>(d.err));
    // Absence is begin()'s story (spec §10 ImuAbsent); a bench probe stays
    // mute and never triggers bus recovery for a merely-missing chip (D13).
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
    TEST_ASSERT_EQUAL_UINT(0, f.bus.recoverCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Uninitialized),
                          static_cast<int>(f.imu.status()));
}
