#include <bw/hal/max30102_ppg.h>

namespace bw::hal {
namespace {

// [HW] Register map (MAX30102 datasheet, register description table).
constexpr uint8_t kRegFifoWrPtr = 0x04;   // 5-bit FIFO write pointer (R/W)
constexpr uint8_t kRegOvfCounter = 0x05;  // lost-sample counter (R/W)
constexpr uint8_t kRegFifoRdPtr = 0x06;   // 5-bit FIFO read pointer (R/W)
constexpr uint8_t kRegFifoData = 0x07;    // read pops one 6-byte sample
constexpr uint8_t kRegFifoConfig = 0x08;
constexpr uint8_t kRegModeConfig = 0x09;
constexpr uint8_t kRegSpO2Config = 0x0A;
constexpr uint8_t kRegLed1Pa = 0x0C;  // red LED pulse amplitude
constexpr uint8_t kRegLed2Pa = 0x0D;  // IR LED pulse amplitude
constexpr uint8_t kRegPartId = 0xFF;

// [HW] FIFO_CONFIG (register 0x08), decomposed field by field:
//   [7:5] SMP_AVE     — 010 = 4-sample averaging. Datasheet (SpO2
//                       Subsystem, Sample Averaging): "adjacent samples (in
//                       each individual channel) can be averaged and
//                       decimated on the chip" — per-channel, so SpO2-mode
//                       red/IR interleaving is NOT mixed by the average.
//   [4]   ROLLOVER_EN — 0 (decision below)
//   [3:0] A_FULL      — 0 (interrupts stay disabled; the driver polls)
// FIX loop 2b (tester parity, developer ground truth 2026-10-14): the
// known-working tester runs SMP_AVE=4 with SpO2 rate 400 sps — a 100 Hz
// averaged output. Averaging N raw samples cuts white noise by sqrt(N) = 2
// while the 0.5–4 Hz pulse band is untouched (every averaged word is still
// a pulse sample; the 25 Hz output keeps the 4 Hz band below Nyquist).
// Tester parity: 100 sps / 4 = 25 FIFO samples/s per channel. Parser stamps
// are 40 ms apart; F02 explicitly selects its matching Tester25Hz profile.
// Rollover decision (problem → alternatives → trade-offs → choice): the
// tester enables FIFO_ROLLOVER_EN ("crucial to prevent FIFO freeze/lockup"
// on its clone). Alternatives: (1) adopt ON — no lockup by construction
// and the freshest data wins, but data loss becomes SILENT (up to
// kPpgFifoDepth samples overwritten) and OVF_COUNTER semantics on this
// clone are unverified; (2) keep OFF — loss stays visible: OVF_COUNTER
// increments, read() returns Err::Overflow, and FIX-A's D17 bounded
// reinit ladder recovers. Evidence for (2): zero post-boot overruns in
// the 90 s healthy bench round (drain ≤ one sensor-task period vs
// ~160 ms of FIFO at 100 Hz/channel), and the clone is known to lie
// about clears (D17) — silent overwrite would mask exactly that.
// Choice: KEEP OFF (honest loss visibility per CR-004; no bench evidence
// of drain starvation). Revisit only if bench shows recurring ovf/read
// failures after the averaging lands.
constexpr uint8_t kFifoSampleAverage4 = 0x40;              // [HW] [7:5] = 010
constexpr uint8_t kFifoConfigValue = kFifoSampleAverage4;  // rollover off, A_FULL = 0

// [HW] SpO2_CONFIG (MAX30102 register 0x0A), decomposed field by field so
// every bit's meaning is reviewable:
//   [6:5] ADC full-scale range — 01 = 4096 nA (15.63 pA/LSB at 18-bit width)
//   [4:2] sample rate         — 001 = 100 raw sps per channel; FIFO averaging
//                               divides by four: 25 Hz = kPpgSampleRateHz.
//   [1:0] pulse width         — 11 = 411 µs, the 18-bit ADC width
//                               (equals kPpgAdcFullScaleCounts' assumption)
// Range history (FIX loops 1-2, 2026-10-14):
//   POR/loop-0 was 00 = 2048 nA — RAILS on the finger's DC photocurrent
//   (bit-identical IR = PpgFlatline, PI ~ 0, HR "--"; handoff
//   ppg_first_light_20261013.md). Loop 1 set 10 = 8192 nA: the rail went
//   away (bench dc ~ 81 500 counts = 2540 nA, sat 0.00) but the pulsatile
//   component stayed undetectable (PI 0.2-0.4 %, 0 beats/90 s, hr never
//   valid; bench_check8.py + hr_finger_round2_20261014.out).
// Why 4096 now (datasheet-verified, loop 2): the range bits select the TIA
// gain (full-scale current = V_FS / R_F; the ADC voltage span is fixed).
// MAX30102 datasheet Table 5: doubling the range doubles the LSB size in
// amperes (7.81 -> 15.63 pA), i.e. a pulse of a given photocurrent yields
// HALF the counts, while the dominant voltage-domain noise floor (TIA
// amplifier + ADC noise, referred to counts) is range-invariant
// [DERIVED] — so every range doubling costs up to 6 dB of pulse SNR and
// can never add any (the smaller range that still fits the DC always wins).
// Headroom re-evaluated against kPpgAdcFullScaleCounts: measured DC
// 2540 nA -> ~163 000 counts = 62 % of full scale at 4096 nA, i.e. it FITS
// — which is exactly why the developer's known-working tester writes 0x27
// (range 4096, same 100 sps / 411 µs / LED 0x24 here, tester
// MAX30102.cpp setupSensor(4,100,3)); this restores byte-for-byte register
// parity with the one configuration proven on this hardware. The [PPGWIN]/
// [PPGPK] bench lines (wire_f02) verify dc headroom and in-band SNR on
// hardware.
constexpr uint8_t kSpO2AdcRange4096na = 0x20;   // [HW] [6:5] = 01
constexpr uint8_t kSpO2SampleRate100hz = 0x04;  // [HW] [4:2] = 001 (÷4 avg = 25 Hz FIFO)
constexpr uint8_t kSpO2PulseWidth411us = 0x03;  // [HW] [1:0] = 11
constexpr uint8_t kSpO2ConfigValue =
    kSpO2AdcRange4096na | kSpO2SampleRate100hz | kSpO2PulseWidth411us;

// [HW] MODE_CONFIG (datasheet register map, register 0x09): bit 6 = RESET —
// writing 1 resets every register to its power-on default and self-clears
// when done. Reset default MODE = 000 (shutdown), so the chip stays quiet
// until the final mode write in begin().
constexpr uint8_t kModeReset = 0x40;

// [HW] MODE_CONFIG MODE = 011 → SpO2 mode = alternating red + IR, the two
// channels PpgSample carries. Written LAST in begin(): the soft reset first
// parks the chip in its shutdown default, then every behaviour-relevant
// register is configured, and sampling starts only with the configuration in
// place. The original choice of "no soft-reset" ("a reset would only add a
// blocking poll loop for no observable difference") was refuted by the
// 2026-10-13 bench: an ESP32 reset does not power-cycle the sensor, so every
// boot inherits a running chip with a full FIFO and a stale overflow counter
// (D17: and the loop-2 soak showed the clone may silently IGNORE some of
// these writes — hence every step below is confirmed by readback).
constexpr uint8_t kModeSpO2 = 0x03;

}  // namespace

bw::Result<void> Max30102Ppg::begin() {
    uint8_t reg = kRegPartId;
    uint8_t id = 0;
    bw::Result<void> probe = bus_.writeRead(kMax30102Addr7, &reg, 1, &id, 1);
    if (!probe.ok()) {
        if (probe.err == bw::Err::NotFound) {
            tracker_.beginAbsent();
            reportEdge(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error);
        }
        return probe;
    }
    if (id != kExpectedMax30102PartId) {
        tracker_.beginAbsent();
        reportEdge(bw::FaultCode::PpgAbsent, bw::FaultSeverity::Error);
        return bw::Result<void>{bw::Err::NotFound};
    }

    // Soft reset first: an ESP32 EN/serial reset leaves the sensor running
    // from the previous session — it sampled through the whole bootloader
    // window, so the FIFO is full and OVF_PTR > 0 at every boot (2026-10-13
    // bench). D17: do not TRUST it happened — read the pointers back and
    // remember what this silicon actually did (a clone that drops the reset
    // write leaves the stale state in place; the clear below adapts).
    bw::Result<void> reset = softReset();
    if (!reset.ok()) {
        return reset;
    }
    uint8_t wr = 0;
    uint8_t ovf = 0;
    uint8_t rd = 0;
    bw::Result<void> hdr = readPointers(wr, ovf, rd);
    if (!hdr.ok()) {
        return hdr;
    }
    diag_.reset_cleared = (wr == 0 && rd == 0 && ovf == 0);
    const bool stale_ovf = (ovf > 0);  // remembered for the one boot report below

    // D17 staged sequence: verified clear (readback — the write alone proves
    // nothing on this silicon), mode last (sampling starts only with the
    // configuration in place, readback-proven), settle drain (the first
    // sensorTask drain is ~100-300 ms away; anything left in the FIFO now is
    // drained and DISCARDED so the gap cannot overrun the 32-sample window,
    // and the whole read path is proven end-to-end in boot context).
    bw::Result<void> staged = configureAndClear();
    if (!staged.ok()) {
        // R1: one bounded soft-reset retry (full reinit) when even the pop
        // could not re-sync the stream — only a still-broken chip fails
        // begin(), and every attempt is attempt-counted, never a spin.
        staged = reinitialize();
        if (!staged.ok()) {
            return staged;
        }
    }

    if (stale_ovf) {
        // Exactly one PpgFifoOverrun at boot: samples were lost while nobody
        // drained (the stale session's). Handled and discarded here; status
        // recovers to Ok immediately (integrator acceptance: one warning,
        // then Ok — never a latch).
        reportEdge(bw::FaultCode::PpgFifoOverrun, bw::FaultSeverity::Warning);
        overrun_reported_ = true;
    }

    tracker_.beginOk();
    absent_reported_ = false;
    episode_ladder_done_ = false;
    return bw::Result<void>{bw::Err::Ok};
}

bw::Result<void> Max30102Ppg::softReset() {
    const uint8_t reset_cmd[2] = {kRegModeConfig, kModeReset};
    bw::Result<void> w = bus_.write(kMax30102Addr7, reset_cmd, sizeof(reset_cmd));
    if (!w.ok()) {
        return w;
    }
    // Poll the self-clearing bit until the reset completes (no delay(): pure
    // driver, every iteration is one bounded bus transaction; the attempt
    // budget is kPpgSoftResetPollAttempts). A read that fails keeps the poll
    // spinning within the bound — configuring registers while the chip is
    // still resetting would let the reset wipe them silently. NACK ends it
    // immediately: the chip vanished mid-begin, absence beats "still waiting".
    // A clone that DROPPED the reset write reads the old MODE value here —
    // bit clear — and the poll exits "successfully"; begin()'s pointer
    // readback is what exposes that case (D17), not this poll.
    for (uint8_t attempt = 0; attempt < kPpgSoftResetPollAttempts; ++attempt) {
        uint8_t reg = kRegModeConfig;
        uint8_t mode = kModeReset;  // non-zero reads as "still resetting" if the transfer fails
        const bw::Result<void> r = bus_.writeRead(kMax30102Addr7, &reg, 1, &mode, 1);
        if (r.ok()) {
            if ((mode & kModeReset) == 0) {
                return bw::Result<void>{bw::Err::Ok};
            }
        } else if (r.err == bw::Err::NotFound) {
            return r;
        }
    }
    // Budget exhausted: the reset did not complete — Timeout means "the
    // bounded wait failed", whatever the last transient error was.
    return bw::Result<void>{bw::Err::Timeout};
}

bw::Result<void> Max30102Ppg::configure() {
    bw::Result<void> cfg = writeVerified(kRegFifoConfig, kFifoConfigValue);
    if (!cfg.ok()) {
        return cfg;
    }
    cfg = writeVerified(kRegSpO2Config, kSpO2ConfigValue);
    if (!cfg.ok()) {
        return cfg;
    }
    cfg = writeVerified(kRegLed1Pa, kPpgLedCurrentPaValue);
    if (!cfg.ok()) {
        return cfg;
    }
    return writeVerified(kRegLed2Pa, kPpgLedCurrentPaValue);
}

bw::Result<void> Max30102Ppg::writeVerified(uint8_t reg, uint8_t value) {
    // D17: every configuration write is confirmed by a readback — the 2026-10
    // clone silently dropped writes, and a register that does not stick means
    // the chip's actual behaviour (rate, width, LED current) is NOT what the
    // driver believes: unusable, reported as HwFault, boot continues (§10).
    const uint8_t cmd[2] = {reg, value};
    bw::Result<void> w = bus_.write(kMax30102Addr7, cmd, sizeof(cmd));
    if (!w.ok()) {
        return w;
    }
    uint8_t got = 0xFF;
    bw::Result<void> r = bus_.writeRead(kMax30102Addr7, &reg, 1, &got, 1);
    if (!r.ok()) {
        return r;
    }
    if (got == value) {
        return bw::Result<void>{bw::Err::Ok};
    }
    // One bounded rewrite absorbs a transient bus glitch; a second mismatch
    // is a chip that does not honor the register (no unbounded retry loops).
    w = bus_.write(kMax30102Addr7, cmd, sizeof(cmd));
    if (!w.ok()) {
        return w;
    }
    r = bus_.writeRead(kMax30102Addr7, &reg, 1, &got, 1);
    if (!r.ok()) {
        return r;
    }
    if (got == value) {
        return bw::Result<void>{bw::Err::Ok};
    }
    return bw::Result<void>{bw::Err::HwFault};
}

bw::Result<void> Max30102Ppg::clearFifo() {
    // WR_PTR, OVF_COUNTER and RD_PTR are one contiguous 3-register block, so
    // a single write attempts to reset pointers and the overrun counter
    // atomically. D17: on the loop-1 silicon this write ACKed but did NOT
    // clear OVF_PTR — callers must verify via readback (clearFifoVerified).
    const uint8_t clear[4] = {kRegFifoWrPtr, 0, 0, 0};
    return bus_.write(kMax30102Addr7, clear, sizeof(clear));
}

bw::Result<void> Max30102Ppg::readPointers(uint8_t& wr_ptr, uint8_t& ovf, uint8_t& rd_ptr) {
    uint8_t reg = kRegFifoWrPtr;
    uint8_t hdr[3] = {0, 0, 0};  // write pointer, overflow counter, read pointer
    bw::Result<void> burst = bus_.writeRead(kMax30102Addr7, &reg, 1, hdr, sizeof(hdr));
    if (!burst.ok()) {
        return burst;
    }
    wr_ptr = hdr[0] & 0x1F;  // 5-bit pointers (parser uses the same mask)
    ovf = hdr[1];
    rd_ptr = hdr[2] & 0x1F;
    return bw::Result<void>{bw::Err::Ok};
}

bw::Result<void> Max30102Ppg::discardFifo() {
    // D17 settle/discard drain: pop exactly the samples the pointers say are
    // there — never read past the read pointer (a clone may keep advancing RD
    // on over-reads, corrupting the next count), and stay within one Wire
    // transfer (count <= 31 → 186 B <= kI2cWireBufferBytes = 256). With an
    // empty FIFO it still performs one probe read of FIFO_DATA so begin()
    // exercises the whole data path end-to-end on the bench, where a failure
    // is visible. Reading FIFO_DATA pops samples by silicon design, so this
    // works even when the pointer-write clear does not (D17 fallback).
    uint8_t wr_ptr = 0;
    uint8_t ovf_counter = 0;
    uint8_t rd_ptr = 0;
    bw::Result<void> hdr = readPointers(wr_ptr, ovf_counter, rd_ptr);
    if (!hdr.ok()) {
        return hdr;
    }
    const size_t available = ppgFifoSampleCount(wr_ptr, rd_ptr);
    const size_t n = (available > 0) ? available : 1;  // probe read when empty
    uint8_t reg = kRegFifoData;
    return bus_.writeRead(kMax30102Addr7, &reg, 1, fifo_bytes_, n * kPpgFifoBytesPerSample);
}

bw::Result<Max30102Ppg::ClearOutcome> Max30102Ppg::clearFifoVerified() {
    // D17: clear the FIFO and PROVE it took. Fast path: the write reads back
    // all-zero (silicon that honors it). Fallback: pop every sample via a
    // FIFO_DATA read until the readback shows WR == RD — consistent by
    // difference even when OVF_PTR is stuck. A stuck counter is reported as
    // StuckCounter, NOT latched here: the caller must first give the bounded
    // soft-reset ladder a chance to restore a clearable counter (R2), and
    // only latch the degradation when the ladder cannot (overrun detection
    // becomes unavailable on this chip — recorded in the spec; samples flow).
    bw::Result<void> w = clearFifo();
    if (!w.ok()) {
        return bw::Result<ClearOutcome>{w.err};
    }
    uint8_t wr = 0;
    uint8_t ovf = 0;
    uint8_t rd = 0;
    bw::Result<void> hdr = readPointers(wr, ovf, rd);
    if (!hdr.ok()) {
        return bw::Result<ClearOutcome>{hdr.err};
    }
    if (wr == 0 && rd == 0 && ovf == 0) {
        diag_.clear_via_write = true;
        return bw::Result<ClearOutcome>{bw::Err::Ok, ClearOutcome::Verified};
    }
    diag_.clear_via_write = false;
    bw::Result<void> popped = discardFifo();
    if (!popped.ok()) {
        return bw::Result<ClearOutcome>{popped.err};
    }
    hdr = readPointers(wr, ovf, rd);
    if (!hdr.ok()) {
        return bw::Result<ClearOutcome>{hdr.err};
    }
    if (wr != rd) {
        // Popping cannot re-sync the pointers either: no software path can
        // drain this chip. Honest hardware fault — the caller counts it.
        return bw::Result<ClearOutcome>{bw::Err::HwFault};
    }
    return bw::Result<ClearOutcome>{bw::Err::Ok, (ovf > 0) ? ClearOutcome::StuckCounter
                                                           : ClearOutcome::Verified};
}

bw::Result<void> Max30102Ppg::configureAndClear() {
    // The staged post-probe sequence (D17), shared by begin() and the R2
    // ladder: configure every behaviour-relevant register (each readback-
    // verified), verified FIFO clear, mode last so sampling restarts only
    // with the configuration in place, settle drain.
    bw::Result<void> cfg = configure();
    if (!cfg.ok()) {
        return cfg;
    }
    bw::Result<ClearOutcome> cleared = clearFifoVerified();
    if (!cleared.ok()) {
        return bw::Result<void>{cleared.err};
    }
    // The chip-level verdict on OVF_PTR: a Verified clear restores overrun
    // detection; a stuck counter after every attempt this sequence controls
    // is the documented degradation (D17) — latch it so read() stops
    // trusting the register and the data path keeps flowing.
    diag_.ovf_stuck = (cleared.value == ClearOutcome::StuckCounter);
    bw::Result<void> mode = writeVerified(kRegModeConfig, kModeSpO2);
    if (!mode.ok()) {
        return mode;
    }
    return discardFifo();
}

bw::Result<void> Max30102Ppg::reinitialize() {
    // R2 ladder step: begin()'s full post-probe sequence — soft reset,
    // reconfigure, verified clear, mode, settle discard — without the part-ID
    // probe (the chip answered microseconds ago). Each stage is bounded
    // (attempt-counted, no delay()); worst case ≈ 15 bus transactions. When
    // the reset is honored, the re-run clear is Verified and ovf_stuck is
    // cleared inside clearFifoVerified: overrun detection is restored.
    bw::Result<void> r = softReset();
    if (!r.ok()) {
        return r;
    }
    return configureAndClear();
}

bw::Result<size_t> Max30102Ppg::read(bw::PpgSample* out, size_t max_samples) {
    if (max_samples == 0 || out == nullptr) {
        // 02 §3: "returns 0 if none new"; no bus traffic for a caller with
        // no room (null out with room is a caller bug — also "no data").
        return bw::Result<size_t>{bw::Err::Ok, 0};
    }

    uint8_t wr_ptr = 0;
    uint8_t ovf_counter = 0;
    uint8_t rd_ptr = 0;
    bw::Result<void> hdr = readPointers(wr_ptr, ovf_counter, rd_ptr);
    if (!hdr.ok()) {
        return noteFailure(hdr.err);
    }

    if (!diag_.ovf_stuck && ovf_counter > 0) {
        // Overrun episode (CR-004): samples were lost — the stream has a
        // gap. D11/D17: an overrun is NOT by itself a device failure; the
        // drain VERIFIES what this silicon actually did and adapts:
        //   1. verified clear (readback) — next drain is clean;
        //   2. bounded reinitialization ladder, once per episode (R2):
        //      soft reset + reconfigure + verified clear + settle;
        //   3. only when even that cannot restore a consistent stream does
        //      the episode count as read failures toward the honest Failed
        //      state — which the kSensorAbsentRetryMs retry recovers from.
        // This replaces both the pre-fix "every overrun counts" latch (one
        // boot episode killed PPG until reboot on hardware) and loop-1's
        // "consecutive drains count" rule (the same, one drain later, when
        // the clear is a permanent no-op on this silicon).
        if (!overrun_reported_) {
            // One PpgFifoOverrun per overrun episode (D7 edge rule).
            reportEdge(bw::FaultCode::PpgFifoOverrun, bw::FaultSeverity::Warning);
            overrun_reported_ = true;
        }
        const bw::Result<ClearOutcome> c = clearFifoVerified();
        if (c.ok() && c.value == ClearOutcome::Verified) {
            episode_ladder_done_ = false;  // stream usable again next drain
            return bw::Result<size_t>{bw::Err::Overflow, 0};
        }
        // Persistent OVF or an unsynced stream: the bounded soft-reset
        // ladder gets its ONE chance for this episode (R2) — a reset the
        // chip honors restores a clearable counter and full detection.
        if (!episode_ladder_done_) {
            episode_ladder_done_ = true;  // cap: the ladder runs once per episode
            for (uint8_t attempt = 0; attempt < kPpgOvfReinitAttemptsPerEpisode; ++attempt) {
                if (diag_.reinit_runs < 0xFFFFu) {  // saturate: the diag counter never wraps
                    ++diag_.reinit_runs;
                }
                if (reinitialize().ok()) {
                    return bw::Result<size_t>{bw::Err::Overflow, 0};
                }
            }
        }
        if (c.ok()) {
            // The stream is usable (WR == RD) even though the counter cannot
            // be cleared and the reset did not help: latch the degradation —
            // from now on the stuck register is ignored and data flows
            // (documented D17 limitation: overrun detection unavailable).
            diag_.ovf_stuck = true;
            episode_ladder_done_ = false;  // episode closed; the data path is open
            return bw::Result<size_t>{bw::Err::Overflow, 0};
        }
        return noteFailure(bw::Err::Overflow);
    }
    // Clean header (or a chip whose stuck OVF register we stopped trusting):
    // any overrun episode is over; drain normally.
    episode_ladder_done_ = false;

    size_t count = ppgFifoSampleCount(wr_ptr, rd_ptr);
    if (count == 0) {
        tracker_.readOk();
        overrun_reported_ = false;  // a clean read re-arms the overrun latch
        return bw::Result<size_t>{bw::Err::Ok, 0};
    }

    const size_t n = (count < max_samples) ? count : max_samples;
    uint8_t reg = kRegFifoData;
    bw::Result<void> burst =
        bus_.writeRead(kMax30102Addr7, &reg, 1, fifo_bytes_, n * kPpgFifoBytesPerSample);
    if (!burst.ok()) {
        return noteFailure(burst.err);
    }

    // Cap handling: only n of `count` samples are read (the rest stay in the
    // FIFO for the next drain), but their timestamps are absolute. The parser
    // anchors at the NEWEST sample of its input, so a capped read pretends
    // the subset is older by exactly the samples it left behind:
    //   t_i = (newest - (count-n)·dt) - (n-1-i)·dt = newest - (count-1-i)·dt.
    // Without this, samples kept on a capped drain would be stamped late by
    // (count-n)·10 ms — wrong for F02's beat timing (D11).
    const bw::TimeMs newest_t_ms = (clock_ != nullptr) ? clock_->nowMs() : 0;
    const bw::TimeMs withheld_ms = static_cast<bw::TimeMs>(count - n) * kPpgSampleIntervalMs;
    const bw::TimeMs anchor_t_ms = (newest_t_ms > withheld_ms) ? (newest_t_ms - withheld_ms) : 0;
    bw::Result<size_t> parsed = parsePpgFifo(fifo_bytes_, n * kPpgFifoBytesPerSample, anchor_t_ms,
                                             kPpgSampleRateHz, out, n);
    if (!parsed.ok()) {
        // InvalidArg here would be a driver bug (whole samples, rate != 0);
        // still counted as a failure so it surfaces instead of hiding.
        return noteFailure(parsed.err);
    }

    tracker_.readOk();
    overrun_reported_ = false;
    absent_reported_ = false;
    return bw::Result<size_t>{bw::Err::Ok, parsed.value};
}

bw::Result<size_t> Max30102Ppg::noteFailure(bw::Err err) {
    // Every non-Ok read counts toward Failed; the transition edge reports
    // once (D8: the driver maps the edge to its own FaultCode).
    if (tracker_.readFail()) {
        reportEdge(bw::FaultCode::PpgReadFailed, bw::FaultSeverity::Error);
    }
    return bw::Result<size_t>{err, 0};
}

void Max30102Ppg::reportEdge(bw::FaultCode code, bw::FaultSeverity severity) {
    if (code == bw::FaultCode::PpgAbsent) {
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
