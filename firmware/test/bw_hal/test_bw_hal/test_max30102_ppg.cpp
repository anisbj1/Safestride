// Host tests for the Max30102Ppg driver (F01 §9: FIFO drain, overflow,
// failure counting — against a scripted register device, F01 D10).

#include <unity.h>

#include <bw/testkit/fake_clock.h>
#include <bw/testkit/recording_fault_reporter.h>

#include <bw/hal/i2c_bus.h>
#include <bw/hal/max30102_ppg.h>

#include "fake_register_device.h"

using bw::FaultCode;
using bw::SensorStatus;
using bw::hal::I2cBus;
using bw::hal::kMax30102Addr7;
using bw::hal::kPpgFifoBytesPerSample;
using bw::hal::kPpgSampleIntervalMs;
using bw::hal::kPpgSampleRateHz;
using bw::hal::Max30102Ppg;
using bw::hal::test::FakeRegisterBus;
using bw::hal::test::FakeRegisterDevice;
using bw::testkit::FakeClock;
using bw::testkit::RecordingFaultReporter;

namespace {

// [HW] Register addresses, same values as max30102_ppg.cpp (datasheet).
constexpr uint8_t kRegFifoWrPtr = 0x04;
constexpr uint8_t kRegOvfCounter = 0x05;
constexpr uint8_t kRegFifoRdPtr = 0x06;

// One FIFO entry: red (3 bytes) + IR (3 bytes), MSB first (18-bit values).
void putSample(uint8_t* dst, uint32_t red, uint32_t ir) {
    dst[0] = static_cast<uint8_t>((red >> 16) & 0x3F);  // top 2 bits unused
    dst[1] = static_cast<uint8_t>((red >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>(red & 0xFF);
    dst[3] = static_cast<uint8_t>((ir >> 16) & 0x3F);
    dst[4] = static_cast<uint8_t>((ir >> 8) & 0xFF);
    dst[5] = static_cast<uint8_t>(ir & 0xFF);
}

struct Fixture {
    FakeRegisterBus bus;
    FakeClock clock;
    RecordingFaultReporter<8> faults;
    I2cBus i2c;
    Max30102Ppg ppg;

    Fixture() : i2c(bus, &clock, &faults), ppg(i2c, &clock, &faults) {
        // Chip powers up with Part ID = 0x15 (datasheet).
        bus.device(kMax30102Addr7).regs[0xFF] = 0x15;
    }
    FakeRegisterDevice& dev() { return bus.device(kMax30102Addr7); }

    // Seed `samples` FIFO entries starting at read pointer `rd`.
    void seedFifo(uint8_t rd, uint8_t samples) {
        FakeRegisterDevice& d = dev();
        d.regs[kRegFifoRdPtr] = rd;
        d.regs[kRegFifoWrPtr] = static_cast<uint8_t>((rd + samples) & 0x1F);
        d.regs[kRegOvfCounter] = 0;
        d.fifo_len = static_cast<size_t>(samples) * kPpgFifoBytesPerSample;
        for (uint8_t i = 0; i < samples; ++i) {
            putSample(d.fifo + static_cast<size_t>(i) * kPpgFifoBytesPerSample, 1000u + i,
                      2000u + i);
        }
    }
};

}  // namespace

void test_ppg_begin_writes_config_and_marks_ok(void) {
    Fixture f;
    bw::Result<void> r = f.ppg.begin();
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_HEX8(0x40, f.dev().regs[0x08]);  // SMP_AVE=4, no rollover
    // FIX 2026-10-14: range was 00 = 2048 nA (railed on finger DC → no HR);
    // Tester parity: 01 = 4096 nA, rate 001 = 100 sps, PW 11 = 411 µs.
    // Literal (not the constant) so reverting the driver fix FAILS here.
    TEST_ASSERT_EQUAL_HEX8(0x27, f.dev().regs[0x0A]);  // 4096 nA, 100 Hz raw, 411 µs
    TEST_ASSERT_EQUAL_UINT32(25, bw::hal::kPpgSampleRateHz);
    TEST_ASSERT_EQUAL_UINT64(40, kPpgSampleIntervalMs);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kPpgLedCurrentPaValue, f.dev().regs[0x0C]);
    TEST_ASSERT_EQUAL_HEX8(bw::hal::kPpgLedCurrentPaValue, f.dev().regs[0x0D]);
    TEST_ASSERT_EQUAL_HEX8(0x03, f.dev().regs[0x09]);  // mode last: SpO2 (red+IR)
    // FIFO cleared at init: pointers and overrun counter zero.
    TEST_ASSERT_EQUAL_HEX8(0, f.dev().regs[kRegFifoWrPtr]);
    TEST_ASSERT_EQUAL_HEX8(0, f.dev().regs[kRegOvfCounter]);
    TEST_ASSERT_EQUAL_HEX8(0, f.dev().regs[kRegFifoRdPtr]);
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
}

void test_ppg_begin_wrong_part_id_reports_absent_once(void) {
    Fixture f;
    f.dev().regs[0xFF] = 0x14;
    bw::Result<void> r = f.ppg.begin();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotFound), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgAbsent));
    r = f.ppg.begin();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgAbsent));
}

void test_ppg_begin_nack_reports_absent(void) {
    Fixture f;
    f.dev().nack = true;
    bw::Result<void> r = f.ppg.begin();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::NotFound), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgAbsent));
}

void test_ppg_read_empty_fifo_returns_zero_without_fifo_traffic(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());
    const size_t before = f.bus.transferCount();

    bw::PpgSample out[4];
    bw::Result<size_t> r = f.ppg.read(out, 4);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(0, r.value);
    // Exactly one transfer: the 3-byte pointer header. No FIFO_DATA read.
    TEST_ASSERT_EQUAL_UINT(before + 1, f.bus.transferCount());
    TEST_ASSERT_EQUAL_UINT(3, f.bus.callAt(before).rx_len);
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
}

void test_ppg_read_drains_samples_with_reconstructed_timestamps(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());
    f.clock.setMs(1000);
    f.seedFifo(0, 3);

    bw::PpgSample out[4];
    bw::Result<size_t> r = f.ppg.read(out, 4);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(3, r.value);
    // Oldest first, 40 ms apart, newest anchored to the clock (D11/§9).
    TEST_ASSERT_EQUAL_UINT64(1000 - 2 * kPpgSampleIntervalMs, out[0].t_ms);
    TEST_ASSERT_EQUAL_UINT64(1000 - kPpgSampleIntervalMs, out[1].t_ms);
    TEST_ASSERT_EQUAL_UINT64(1000, out[2].t_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, out[0].red);
    TEST_ASSERT_EQUAL_UINT32(2000, out[0].ir);
    TEST_ASSERT_EQUAL_UINT32(1002, out[2].red);
    TEST_ASSERT_FALSE(out[0].saturated);

    // FIFO drained: the next call finds nothing (model advanced the pointer).
    r = f.ppg.read(out, 4);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(0, r.value);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
}

void test_ppg_read_caps_but_keeps_absolute_timestamps(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());
    f.clock.setMs(1000);
    f.seedFifo(0, 5);  // ages 160..0 ms: t = 840, 880, 920, 960, 1000

    bw::PpgSample out[5];
    bw::Result<size_t> r = f.ppg.read(out, 2);  // caller has room for 2
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(2, r.value);
    // The two OLDEST with their true absolute stamps — not shifted to look
    // like a fresh stream (withheld samples re-stamp correctly below).
    TEST_ASSERT_EQUAL_UINT64(840, out[0].t_ms);
    TEST_ASSERT_EQUAL_UINT64(880, out[1].t_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, out[0].red);
    TEST_ASSERT_EQUAL_UINT32(1001, out[1].red);

    // The remaining 3 are still in the FIFO and drain next call.
    r = f.ppg.read(out, 5);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(3, r.value);
    TEST_ASSERT_EQUAL_UINT64(920, out[0].t_ms);
    TEST_ASSERT_EQUAL_UINT64(960, out[1].t_ms);
    TEST_ASSERT_EQUAL_UINT64(1000, out[2].t_ms);
    TEST_ASSERT_EQUAL_UINT32(1002, out[0].red);
    TEST_ASSERT_EQUAL_UINT32(1004, out[2].red);
}

void test_ppg_fifo_overrun_reports_once_per_episode(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());

    f.dev().regs[kRegOvfCounter] = 1;  // FIFO was not drained in time
    bw::PpgSample out[4];
    bw::Result<size_t> r = f.ppg.read(out, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow),
                          static_cast<int>(r.err));  // CR-004: data was lost
    TEST_ASSERT_EQUAL_UINT(0, r.value);
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));
    // The overrun counter was cleared so the chip can start over.
    TEST_ASSERT_EQUAL_HEX8(0, f.dev().regs[kRegOvfCounter]);

    // Second overrun inside the same episode: still one report (D7 edge).
    f.dev().regs[kRegOvfCounter] = 1;
    r = f.ppg.read(out, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));

    // A clean read re-arms the latch -> the next episode reports again.
    r = f.ppg.read(out, 4);
    TEST_ASSERT_TRUE(r.ok());
    f.dev().regs[kRegOvfCounter] = 1;
    r = f.ppg.read(out, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(2, f.faults.countOf(FaultCode::PpgFifoOverrun));
}

// 2026-10-13 hardware regression (FIX-A, evidence: ppg_first_light_20261013.md).
// An ESP32 reset does NOT power-cycle the MAX30102: at every boot the chip was
// still sampling from the previous session — full FIFO, stale OVF_PTR that the
// FIFO-clear register write did not zero. begin()'s soft reset is the only
// mechanism guaranteed to clear that state, so the first drains of a cold boot
// must be clean: no overrun fault, no read fault, ppg=Ok.
void test_ppg_boot_leftover_overrun_recovers_to_ok(void) {
    Fixture f;
    f.dev().ovf_write_ignored = true;  // like the silicon: the clear write cannot zero OVF_PTR
    f.dev().regs[kRegOvfCounter] = 3;  // stale counter inherited from the previous session
    bw::Result<void> r = f.ppg.begin();
    TEST_ASSERT_TRUE(r.ok());
    // The reset applied power-on defaults before configuration: nothing stale remains.
    TEST_ASSERT_EQUAL_HEX8(0, f.dev().regs[kRegOvfCounter]);

    // The boot drain timeline (20 ms task tick): every read clean, Ok, silent.
    bw::PpgSample out[4];
    for (int i = 0; i < 5; ++i) {
        bw::Result<size_t> d = f.ppg.read(out, 4);
        TEST_ASSERT_TRUE(d.ok());
        TEST_ASSERT_EQUAL_UINT(0, d.value);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
    }
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
}

void test_ppg_begin_soft_resets_and_waits_before_configuring(void) {
    Fixture f;
    f.dev().reset_hold_polls = 3;               // chip busy resetting for 3 polls
    f.dev().regs[kRegOvfCounter] = 7;           // stale state from a previous session
    f.dev().fifo_len = kPpgFifoBytesPerSample;  // + one stale sample in the FIFO
    bw::Result<void> r = f.ppg.begin();
    TEST_ASSERT_TRUE(r.ok());
    // Stale state is gone (chip-level reset applied power-on defaults)...
    TEST_ASSERT_EQUAL_HEX8(0, f.dev().regs[kRegOvfCounter]);
    TEST_ASSERT_EQUAL_UINT(0, f.dev().fifo_len);
    // ...and the configuration landed AFTER the reset: mode = SpO2 with the
    // reset bit clear proves begin() polled until completion instead of
    // writing registers into a chip that was about to wipe them.
    TEST_ASSERT_EQUAL_HEX8(0x03, f.dev().regs[0x09]);
    TEST_ASSERT_EQUAL_HEX8(0x40, f.dev().regs[0x08]);  // FIFO_CONFIG: SMP_AVE=4, survived
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.count());
}

void test_ppg_begin_stuck_reset_fails_bounded(void) {
    Fixture f;
    f.dev().reset_hold_polls = 200;  // the chip never finishes resetting
    bw::Result<void> r = f.ppg.begin();
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Timeout), static_cast<int>(r.err));
    // Bounded work: part-ID probe + reset write + exactly the poll budget —
    // no unbounded spin anywhere on the boot path.
    TEST_ASSERT_EQUAL_UINT(2u + bw::hal::kPpgSoftResetPollAttempts, f.bus.transferCount());
    // Unconfigured device: nothing claims Ok, and a timeout is not absence.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Uninitialized),
                          static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgAbsent));
}

// Mid-stream episode on a chip whose clear write stopped taking but which
// still honors the soft reset (2026-10-13 loop-2 silicon model): the drain
// discovers the failed clear by readback, the bounded ladder recovers the
// stream with a chip-level reset in the SAME drain — one Overflow, one
// Warning, no failure counted, and detection fully restored afterwards.
void test_ppg_overrun_episode_does_not_latch_failed(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());

    f.dev().ovf_write_ignored = true;
    f.dev().regs[kRegOvfCounter] = 2;
    bw::PpgSample out[4];
    bw::Result<size_t> r = f.ppg.read(out, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow),
                          static_cast<int>(r.err));  // CR-004: data was lost
    TEST_ASSERT_EQUAL_UINT(0, r.value);
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));
    // The ladder restored a clearable counter: no stuck degradation latched.
    TEST_ASSERT_EQUAL_UINT(1, f.ppg.diag().reinit_runs);
    TEST_ASSERT_FALSE(f.ppg.diag().ovf_stuck);
    TEST_ASSERT_EQUAL_HEX8(0, f.dev().regs[kRegOvfCounter]);

    // The pre-fix bug latched Failed on the 5th overrun drain; now every
    // following drain is a clean Ok and no ReadFailed ever appears.
    for (int i = 0; i < 5; ++i) {
        r = f.ppg.read(out, 4);
        TEST_ASSERT_TRUE(r.ok());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
        TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgReadFailed));
    }
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));  // one per episode

    // A second episode reports again (D7 edge re-armed by the clean drains)
    // and recovers the same way — recovery, never a latch.
    f.dev().ovf_write_ignored = true;
    f.dev().regs[kRegOvfCounter] = 1;
    r = f.ppg.read(out, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(2, f.faults.countOf(FaultCode::PpgFifoOverrun));
    TEST_ASSERT_EQUAL_UINT(2, f.ppg.diag().reinit_runs);
    f.seedFifo(0, 2);
    r = f.ppg.read(out, 4);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(2, r.value);
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgReadFailed));
}

// A chip where NOTHING re-syncs the stream (reset write dropped, pointer and
// OVF writes ignored, FIFO_DATA reads never advance RD) cannot be recovered
// by any ladder: the episode is honest about it — one Warning, one
// PpgReadFailed when the failure budget runs out, Failed reached — and even
// that is not a permanent latch: a responding chip recovers to Ok. The
// ladder's budget runs exactly once for the episode (R2 cap), counted in
// diag().reinit_runs.
void test_ppg_persistent_overrun_reaches_failed_and_recovers(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());

    // Dead silicon from here on: every write ignored, reads never drain.
    f.dev().reset_ignored = true;
    f.dev().ovf_write_ignored = true;
    f.dev().pointer_writes_ignored = true;
    f.dev().fifo_pop_broken = true;
    f.seedFifo(0, 4);  // WR=4, RD=0: pops cannot move RD, writes cannot zero it
    f.dev().regs[kRegOvfCounter] = 1;

    bw::PpgSample out[4];
    // Reads 1..4: each Overflow, counting toward the honest Failed state.
    for (int i = 0; i < 4; ++i) {
        bw::Result<size_t> r = f.ppg.read(out, 4);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(r.err));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
        TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgReadFailed));
    }
    bw::Result<size_t> r = f.ppg.read(out, 4);  // read 5: budget exhausted — device-level
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgReadFailed));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));  // one per episode
    TEST_ASSERT_EQUAL_UINT(bw::hal::kPpgOvfReinitAttemptsPerEpisode,
                           f.ppg.diag().reinit_runs);  // bounded, once per episode

    // "Replug": the chip honors writes again and answers with fresh samples.
    f.dev().reset_ignored = false;
    f.dev().ovf_write_ignored = false;
    f.dev().pointer_writes_ignored = false;
    f.dev().fifo_pop_broken = false;
    f.seedFifo(0, 1);
    r = f.ppg.read(out, 4);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(1, r.value);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgReadFailed));  // no repeat report
    TEST_ASSERT_EQUAL_UINT(bw::hal::kPpgOvfReinitAttemptsPerEpisode,
                           f.ppg.diag().reinit_runs);  // recovery ran no ladder
}

// 2026-10-13 loop-2 hardware regression (65 s soak on COM14): the clone
// drops the RESET write entirely and no write clears OVF_PTR, so every boot
// inherits a running chip with a stale counter that no clear ever zeroes.
// begin() must discover that by readback, pop the FIFO to consistency, and
// every boot drain must be clean: one PpgFifoOverrun for the stale episode,
// ppg=Ok sustained, no PpgReadFailed. The loop-1 build failed exactly here:
// Failed at the 6th drain because the never-clearing counter kept counting.
void test_ppg_begin_clone_ignoring_writes_recovers_boot_overrun(void) {
    Fixture f;
    FakeRegisterDevice& d = f.dev();
    d.reset_ignored = true;      // the RESET write is dropped entirely
    d.ovf_write_ignored = true;  // OVF_PTR cannot be cleared by any write
    // Stale state inherited from the previous session (the chip kept sampling
    // through the bootloader window): mode = SpO2, FIFO partly full, counter > 0.
    d.regs[0x09] = 0x03;
    f.seedFifo(10, 6);
    d.regs[kRegOvfCounter] = 5;

    bw::Result<void> r = f.ppg.begin();
    TEST_ASSERT_TRUE(r.ok());                       // begin adapted instead of trusting the writes
    TEST_ASSERT_FALSE(f.ppg.diag().reset_cleared);  // readback proves the lie
    TEST_ASSERT_FALSE(f.ppg.diag().clear_via_write);
    TEST_ASSERT_TRUE(f.ppg.diag().ovf_stuck);  // detection unavailable, data not
    // Exactly one Warning for the stale episode; status recovers immediately.
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgReadFailed));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));

    // The boot drain timeline (20 ms task tick): every read clean — the
    // stuck counter is ignored, never a failure, never a latch.
    bw::PpgSample out[4];
    for (int i = 0; i < 8; ++i) {
        bw::Result<size_t> clean = f.ppg.read(out, 4);
        TEST_ASSERT_TRUE(clean.ok());
        TEST_ASSERT_EQUAL_UINT(0, clean.value);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
    }
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgReadFailed));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));  // boot episode only

    // Fresh samples still flow even though OVF_PTR keeps reading 5.
    f.seedFifo(0, 3);
    d.regs[kRegOvfCounter] = 5;
    bw::Result<size_t> flow = f.ppg.read(out, 4);
    TEST_ASSERT_TRUE(flow.ok());
    TEST_ASSERT_EQUAL_UINT(3, flow.value);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
}

// Mid-session version of the clone case: the chip began clean (reset worked
// then), and later BOTH the clear write and the reset stop being honored.
// The ladder gets its one chance, cannot restore a clearable counter, and
// the driver latches the documented degradation: data keeps flowing with the
// stuck register ignored — never a latch, never a failure.
void test_ppg_runtime_stuck_ovf_adapts_and_keeps_data_flowing(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());

    f.dev().reset_ignored = true;      // mid-session: the clone stops honoring
    f.dev().ovf_write_ignored = true;  // ...both the reset and the clear write
    f.seedFifo(0, 2);
    f.dev().regs[kRegOvfCounter] = 1;

    bw::PpgSample out[4];
    bw::Result<size_t> r = f.ppg.read(out, 4);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(r.err));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));
    TEST_ASSERT_EQUAL_UINT(1, f.ppg.diag().reinit_runs);  // ladder tried once...
    TEST_ASSERT_TRUE(f.ppg.diag().ovf_stuck);             // ...could not clear: degraded, not dead
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgReadFailed));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));

    // Every following drain is clean Ok even though the counter stays > 0.
    f.seedFifo(0, 3);
    f.dev().regs[kRegOvfCounter] = 1;
    for (int i = 0; i < 5; ++i) {
        r = f.ppg.read(out, 4);
        TEST_ASSERT_TRUE(r.ok());
        TEST_ASSERT_EQUAL_UINT(3, r.value);  // data flows: 3 fresh samples each drain
        TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Ok), static_cast<int>(f.ppg.status()));
        TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgReadFailed));
        f.seedFifo(0, 3);  // the chip keeps producing samples
        f.dev().regs[kRegOvfCounter] = 1;
    }
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgFifoOverrun));  // one per episode
    TEST_ASSERT_EQUAL_UINT(1, f.ppg.diag().reinit_runs);  // no ladder re-runs while stuck
}

void test_ppg_read_nack_failures_report_read_failed_once(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());
    f.dev().nack = true;

    bw::PpgSample out[4];
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_FALSE(f.ppg.read(out, 4).ok());
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SensorStatus::Failed), static_cast<int>(f.ppg.status()));
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgReadFailed));
    TEST_ASSERT_FALSE(f.ppg.read(out, 4).ok());
    TEST_ASSERT_EQUAL_UINT(1, f.faults.countOf(FaultCode::PpgReadFailed));
    // Unplugged device is a read failure, not "absent at boot" (fault.h).
    TEST_ASSERT_EQUAL_UINT(0, f.faults.countOf(FaultCode::PpgAbsent));
}

void test_ppg_read_zero_max_samples_does_not_touch_the_bus(void) {
    Fixture f;
    TEST_ASSERT_TRUE(f.ppg.begin().ok());
    const size_t before = f.bus.transferCount();
    bw::Result<size_t> r = f.ppg.read(nullptr, 0);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT(0, r.value);
    TEST_ASSERT_EQUAL_UINT(before, f.bus.transferCount());
}
