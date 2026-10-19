#pragma once

// bw/hal/max30102_ppg.h — MAX30102 (MH-ET Live) driver behind bw::IPpgSensor
// (F01). Pure register protocol on top of I2cBus: the full begin/drain/
// overflow/failure behaviour runs on the host with a scripted register device
// (F01 D10, spec §9). Only I2cBus touches the bus (00 §3). Red + IR only;
// no SpO₂ maths here — raw samples only (00 §3, spec §5).

#include <cstddef>
#include <cstdint>

#include <bw/clock.h>
#include <bw/fault.h>
#include <bw/hardware.h>
#include <bw/result.h>
#include <bw/types.h>

#include <bw/hal/i2c_bus.h>
#include <bw/hal/max30102_parser.h>
#include <bw/hal/sensor_status.h>

namespace bw::hal {

// [HW] 7-bit I²C address (MAX30102 datasheet; fixed by the module).
constexpr uint8_t kMax30102Addr7 = 0x57;

// [HW] Part ID register 0xFF reads 0x15 (identical on MAX30102/05 — the ID
// only proves "the chip answers", the address pins it to this board's part).
constexpr uint8_t kExpectedMax30102PartId = 0x15;

// [DERIVED] Developer's working tester: 100 raw conversions/s with four-sample
// FIFO averaging gives 25 samples/s per channel (not 100 FIFO samples/s).
constexpr uint16_t kPpgSampleRateHz = 25;

// [DERIVED] Spacing between consecutive FIFO samples at kPpgSampleRateHz
// (1000/25 = 40 ms; exact, no rounding residue). Used to
// anchor partial drains (see read(): a capped read still stamps samples at
// their true absolute time instead of pretending the subset is the stream).
constexpr bw::TimeMs kPpgSampleIntervalMs = 1000u / kPpgSampleRateHz;

// [DERIVED] Maximum time between FIFO drains (F01 §8): the FIFO holds
// kPpgFifoDepth = 32 samples = 1280 ms at 25 Hz; polling at ≤ 100 ms leaves
// >12× margin. Enforced by the SensorTask period (wire_hal), checked here as
// the documentation of that duty.
constexpr bw::TimeMs kPpgFifoPollMaxIntervalMs = 100;

// [EXPERIMENTAL] LED current start value (F01 §8 kPpgLedCurrentMa ≈ 7 mA):
// the LED pulse-amplitude register spans 0x00 = 0 mA to 0xFF ≈ 50 mA, i.e.
// ≈ 0.196 mA per code (MAX30102 datasheet, "LED Pulse Amplitude"), so
// 7 mA ≈ 35.7 → 36 codes. Start low to save power; F23 reports saturation /
// low amplitude so this can be tuned with data (F28/F29).
constexpr uint8_t kPpgLedCurrentPaValue = 36;

// [EXPERIMENTAL] Poll budget for the MODE_CONFIG soft-reset bit to self-clear
// in begin(). Why: an ESP32 EN/serial reset does not power-cycle the sensor
// (2026-10-13 bench, evidence ppg_first_light_20261013.md) — the chip is left
// sampling by the previous session, and only a chip-level reset returns its
// FIFO pointers and overflow counter to defaults. Each poll is one register
// read (~0.3 ms at 400 kHz incl. lock), so 64 attempts ≈ 20 ms against the
// typically few-ms reset (community libraries budget 100 ms); one-time at
// boot inside wire(), never in a task loop. Tune with bench data if a chip
// ever times out here.
constexpr uint8_t kPpgSoftResetPollAttempts = 64;

// [EXPERIMENTAL] Bound on the runtime soft-reset reinitialization ladder
// (D17/R2): when the verified FIFO clear fails at runtime, begin()'s full
// sequence (soft reset + reconfigure + verified clear + settle discard) is
// retried at most this many times per overrun episode, on the episode's first
// drain only. Why 2: one attempt absorbs a transient, the second confirms a
// persistent condition without stalling SensorTask (each run is ≈12 bounded
// bus transactions ≈ 5 ms, no delay()); after the budget the episode counts
// as read failures toward the honest Failed state, which the kSensorAbsentRetryMs
// retry still recovers from. Tune with bench data.
constexpr uint8_t kPpgOvfReinitAttemptsPerEpisode = 2;

// Read-only facts latched by the driver for bench discrimination (D17): the
// 2026-10-13 hardware needed to tell "reset honored / clear honored / OVF
// stuck" apart on serial without new composition-root code — f01::imuDiag
// appends these. No side effects; same pattern as Mpu6050Diag (D16).
struct Max30102PpgDiag {
    bool reset_cleared;    // register readback right after the reset poll was all-zero:
                           // false = the chip ignored (or did not honor) the reset write
    bool clear_via_write;  // the 3-register clear write verifiably zeroes pointers + OVF
    bool ovf_stuck;        // OVF_PTR proved permanently non-zero; ignored from then on
                           // (overrun detection unavailable on this chip — D17)
    uint16_t reinit_runs;  // bounded runtime reinitializations executed (R2 ladder)
};

class Max30102Ppg final : public bw::IPpgSensor {
public:
    // `clock` and `faults` may be nullptr (F01 D7/D9 fallbacks): samples are
    // stamped 0 and fault reports are dropped; all other behaviour is the same.
    Max30102Ppg(I2cBus& bus, const bw::IClock* clock, bw::IFaultReporter* faults)
        : bus_(bus), clock_(clock), faults_(faults) {}

    // Part-ID probe + verified soft reset + configuration + verified FIFO
    // clear + settle drain. Every step is CONFIRMED by register readback
    // (D17): the 2026-10-13 clone ignored some writes, and a reset/clear the
    // chip silently drops must be discovered here, in boot context, and
    // adapted to (pop-consistent drain / ovf_stuck_) instead of latching
    // Failed on the first boot drain. Err::NotFound (NACK or wrong ID) marks
    // the device Failed and reports PpgAbsent once per absent episode (§10).
    bw::Result<void> begin() override;

    // Drains up to max_samples FIFO entries into out with reconstructed
    // timestamps (02 §3); Ok/0 when the FIFO is empty. Returns Err::Overflow
    // when the FIFO overran (CR-004: data was lost) after reporting
    // PpgFifoOverrun once per overrun episode (F01 D11). An episodic overrun
    // is a stream gap, not a device failure (D11/D17): the drain VERIFIES
    // whether the clear took (readback), falls back to a pop-consistent
    // drain and a bounded reinitialization ladder, and only counts read
    // failures when even that cannot restore a consistent stream.
    bw::Result<size_t> read(bw::PpgSample* out, size_t max_samples) override;

    bw::SensorStatus status() const override { return tracker_.status(); }

    // Re-run begin(); when to call it belongs to F26 (02 §3).
    bw::Result<void> reinit() override { return begin(); }

    // Latched bench-discrimination facts (D17); no side effects (D16 pattern).
    const Max30102PpgDiag& diag() const { return diag_; }

private:
    // Outcome of clearFifoVerified() when the stream can be re-synced:
    //  Verified          — the 3-register write verifiably zeroed pointers+OVF
    //                      (or the pop did and the counter reads 0): overrun
    //                      detection fully works.
    //  StuckCounter      — pop made the stream consistent (WR == RD) but
    //                      OVF_PTR still reads > 0: the counter cannot be
    //                      cleared by any write this driver controls.
    enum class ClearOutcome : uint8_t { Verified, StuckCounter };

    bw::Result<size_t> noteFailure(bw::Err err);
    bw::Result<void> clearFifo();
    bw::Result<void> softReset();
    bw::Result<void> configure();
    bw::Result<void> writeVerified(uint8_t reg, uint8_t value);
    bw::Result<void> readPointers(uint8_t& wr_ptr, uint8_t& ovf, uint8_t& rd_ptr);
    bw::Result<ClearOutcome> clearFifoVerified();
    bw::Result<void> discardFifo();
    bw::Result<void> configureAndClear();  // configure + verified clear + mode + settle
    bw::Result<void> reinitialize();       // softReset + configureAndClear (R2 ladder)
    void reportEdge(bw::FaultCode code, bw::FaultSeverity severity);

    I2cBus& bus_;
    const bw::IClock* clock_;
    bw::IFaultReporter* faults_;
    SensorStatusTracker tracker_{kFailuresBeforeFailed};
    // One drain at most: kPpgFifoDepth samples × kPpgFifoBytesPerSample (§11).
    uint8_t fifo_bytes_[kPpgFifoDepth * kPpgFifoBytesPerSample] = {};
    bool overrun_reported_ = false;     // one PpgFifoOverrun per episode (D7 rule)
    bool absent_reported_ = false;      // one PpgAbsent per absent episode (D7)
    bool episode_ladder_done_ = false;  // the R2 ladder ran once for the open episode
    Max30102PpgDiag diag_ = {};
};

}  // namespace bw::hal
