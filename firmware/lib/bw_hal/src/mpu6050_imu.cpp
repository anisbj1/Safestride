#include <bw/hal/mpu6050_imu.h>

namespace bw::hal {
namespace {

// [HW] Register map (MPU6050 Register Map and Descriptions).
constexpr uint8_t kRegSampleRateDiv = 0x19;
constexpr uint8_t kRegConfig = 0x1A;
constexpr uint8_t kRegGyroConfig = 0x1B;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegAccelXoutH = 0x3B;
constexpr uint8_t kRegPowerMgmt1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;

// [HW] PWR_MGMT_1 value: SLEEP=0 (the device powers up asleep), CLKSEL=1
// (PLL with X-axis gyro reference) — the datasheet's recommended clock.
constexpr uint8_t kPowerMgmt1Wake = 0x01;

// [HW] Burst read ACCEL_XOUT_H (0x3B) .. GYRO_ZOUT_L (0x4E): accel 6 bytes +
// temperature 2 bytes (discarded: ImuSample carries no temperature) + gyro 6
// bytes, all big-endian (mpu6050_convert be16).
constexpr size_t kBurstBytes = 14;
constexpr size_t kGyroByteOffset = 8;

// [HW] Accel user-offset registers XA_OFFS_H (0x06) .. ZA_OFFS_L (0x0B),
// big-endian signed pairs — read only by readRawDiagnostics (D16 offset
// hypothesis); never written (calibration is F02's, spec §12).
constexpr uint8_t kRegAccelXoffsH = 0x06;
constexpr size_t kAccelOffsetBytes = 6;

// [HW] Contiguous config block 0x19..0x1C (SMPLRT_DIV, CONFIG, GYRO_CONFIG,
// ACCEL_CONFIG) — one read echoes every rate/range register begin() wrote.
constexpr size_t kConfigBlockBytes = 4;

}  // namespace

bw::Result<void> Mpu6050Imu::begin() {
    uint8_t reg = kRegWhoAmI;
    uint8_t id = 0;
    bw::Result<void> probe = bus_.writeRead(kMpu6050Addr7, &reg, 1, &id, 1);
    if (!probe.ok()) {
        // NACK = the chip is not there (A-05: also reached on a clone whose
        // bus answer differs). Bus-level errors (Timeout/BusError) were
        // already reported by I2cBus (D7) — status stays Uninitialized so
        // reinit() remains available (spec §10).
        if (probe.err == bw::Err::NotFound) {
            tracker_.beginAbsent();
            reportEdge(bw::FaultCode::ImuAbsent, bw::FaultSeverity::Error);
        }
        return probe;
    }
    if (id != kExpectedMpuWhoAmI) {
        // A-05: WHO_AM_I mismatch is not silently accepted — same path as
        // absent (fault.h maps it to ImuAbsent).
        tracker_.beginAbsent();
        reportEdge(bw::FaultCode::ImuAbsent, bw::FaultSeverity::Error);
        return bw::Result<void>{bw::Err::NotFound};
    }

    const uint8_t wake[2] = {kRegPowerMgmt1, kPowerMgmt1Wake};
    bw::Result<void> cfg = bus_.write(kMpu6050Addr7, wake, sizeof(wake));
    if (!cfg.ok()) {
        return cfg;
    }

    // One contiguous burst keeps the configured state atomic-ish: divider,
    // DLPF, gyro range, accel range (constants in mpu6050_imu.h).
    const uint8_t config[5] = {kRegSampleRateDiv, kImuSampleRateDiv, kImuDlpfCfg,
                               kImuGyroConfigValue, kImuAccelConfigValue};
    cfg = bus_.write(kMpu6050Addr7, config, sizeof(config));
    if (!cfg.ok()) {
        return cfg;
    }

    tracker_.beginOk();
    absent_reported_ = false;
    return bw::Result<void>{bw::Err::Ok};
}

bw::Result<size_t> Mpu6050Imu::read(bw::ImuSample* out, size_t max_samples) {
    if (max_samples == 0 || out == nullptr) {
        // 02 §3: "returns 0 if none new"; no bus traffic for a caller with no
        // room (a null out with room is a caller bug — also treated as none).
        return bw::Result<size_t>{bw::Err::Ok, 0};
    }

    uint8_t reg = kRegAccelXoutH;
    uint8_t buf[kBurstBytes];
    bw::Result<void> burst = bus_.writeRead(kMpu6050Addr7, &reg, 1, buf, sizeof(buf));
    if (!burst.ok()) {
        // Any failed read counts toward Failed; the transition edge reports
        // once (D7/D8 rate limiting). NACK after a successful begin means the
        // device disappeared mid-run — same symptom family, ImuReadFailed
        // (fault.h reserves ImuAbsent for boot/ID, per its comment).
        if (tracker_.readFail()) {
            reportEdge(bw::FaultCode::ImuReadFailed, bw::FaultSeverity::Error);
        }
        return bw::Result<size_t>{burst.err, 0};
    }

    ImuRaw raw{};
    for (int i = 0; i < 3; ++i) {
        raw.accel[i] = be16(buf + static_cast<size_t>(i) * 2);
        raw.gyro[i] = be16(buf + kGyroByteOffset + static_cast<size_t>(i) * 2);
    }
    const bw::TimeMs t_ms = (clock_ != nullptr) ? clock_->nowMs() : 0;
    out[0] = convertImuRaw(raw, t_ms, kImuAccelRange, kImuGyroRange);
    tracker_.readOk();
    absent_reported_ = false;  // answering again -> the next absence re-arms (D7)
    return bw::Result<size_t>{bw::Err::Ok, 1};
}

bw::Result<Mpu6050Diag> Mpu6050Imu::readRawDiagnostics() {
    Mpu6050Diag diag{};

    // Data burst first: if this fails, the device/bus is the story and the
    // readbacks would say the same thing — one error, no partial guessing.
    uint8_t reg = kRegAccelXoutH;
    uint8_t burst[kBurstBytes];
    bw::Result<void> r = bus_.writeRead(kMpu6050Addr7, &reg, 1, burst, sizeof(burst));
    if (!r.ok()) {
        return bw::Result<Mpu6050Diag>{r.err, diag};
    }
    for (int i = 0; i < 3; ++i) {
        diag.accel_counts[i] = be16(burst + static_cast<size_t>(i) * 2);
        diag.gyro_counts[i] = be16(burst + kGyroByteOffset + static_cast<size_t>(i) * 2);
    }

    uint8_t offsets[kAccelOffsetBytes];
    reg = kRegAccelXoffsH;
    r = bus_.writeRead(kMpu6050Addr7, &reg, 1, offsets, sizeof(offsets));
    if (!r.ok()) {
        return bw::Result<Mpu6050Diag>{r.err, diag};
    }
    for (int i = 0; i < 3; ++i) {
        diag.accel_offset[i] = be16(offsets + static_cast<size_t>(i) * 2);
    }

    uint8_t config_block[kConfigBlockBytes];
    reg = kRegSampleRateDiv;
    r = bus_.writeRead(kMpu6050Addr7, &reg, 1, config_block, sizeof(config_block));
    if (!r.ok()) {
        return bw::Result<Mpu6050Diag>{r.err, diag};
    }
    diag.sample_rate_div = config_block[0];
    diag.config = config_block[1];
    diag.gyro_config = config_block[2];
    diag.accel_config = config_block[3];

    reg = kRegPowerMgmt1;
    r = bus_.writeRead(kMpu6050Addr7, &reg, 1, &diag.power_mgmt1, 1);
    if (!r.ok()) {
        return bw::Result<Mpu6050Diag>{r.err, diag};
    }

    reg = kRegWhoAmI;
    r = bus_.writeRead(kMpu6050Addr7, &reg, 1, &diag.whoami, 1);
    if (!r.ok()) {
        return bw::Result<Mpu6050Diag>{r.err, diag};
    }

    // Deliberately no tracker_ / reportEdge() calls (D16): this is a probe,
    // not a sample — a failed diag must not move the failure streak it may
    // be diagnosing, and a successful one must not reset it.
    return bw::Result<Mpu6050Diag>{bw::Err::Ok, diag};
}

void Mpu6050Imu::reportEdge(bw::FaultCode code, bw::FaultSeverity severity) {
    if (code == bw::FaultCode::ImuAbsent) {
        if (absent_reported_) {
            return;  // one report per absent episode; beginOk() re-arms (D7)
        }
        absent_reported_ = true;
    }
    if (faults_ == nullptr) {
        return;
    }
    const bw::TimeMs t_ms = (clock_ != nullptr) ? clock_->nowMs() : 0;
    faults_->report(code, severity, t_ms);
}

}  // namespace bw::hal
