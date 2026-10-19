#pragma once

// bw/research/research_config.h — F28 M1 named constants (spec §8, D2/D4/D6/D10).
// Every value carries exactly one tag and a why (00 §7.5, named-constant guidance).

#include <cstddef>
#include <cstdint>

// Build-time capture knob (spec §15 A3): enables a capture build WITHOUT
// editing the repo — set on the command line for one build only, e.g.
//   PowerShell: $env:PLATFORMIO_BUILD_FLAGS="-DF28_RESEARCH_CAPTURE_ENABLED=1"
//   cmd:        set PLATFORMIO_BUILD_FLAGS=-DF28_RESEARCH_CAPTURE_ENABLED=1
// PlatformIO appends PLATFORMIO_BUILD_FLAGS to the env's build_flags
// (project/options.py `sysenvvar`, project/config.py `multiple` append), so
// the platformio.ini flags stay intact. Unset the variable afterwards and the
// next build is the repo default (0 = capture off). 0 or 1 only.
#ifndef F28_RESEARCH_CAPTURE_ENABLED
#define F28_RESEARCH_CAPTURE_ENABLED 0
#endif

static_assert(F28_RESEARCH_CAPTURE_ENABLED == 0 || F28_RESEARCH_CAPTURE_ENABLED == 1,
              "F28_RESEARCH_CAPTURE_ENABLED is a 0/1 build flag (spec §15 A3)");

namespace bw::research {

// --- transport ---------------------------------------------------------------

// [DERIVED] F28 D2: 50 Hz × 6 + 100 Hz × 2 values ≈ 500 values/s ≈ 6–10 KB/s of
// CSV; 115200 delivers ≈ 11 KB/s with framing overhead — too close to sustain
// with diagnostics interleaved. 921600 delivers ≈ 92 KB/s; worst-case M1 load
// (5.4 KB/s with the raw tee on) uses ≈ 6 % of the line.
constexpr uint32_t kSerialBaud = 921600;

// --- gating (F28 D4) ---------------------------------------------------------

// [USER] The explicit-enable knob §5 asks for while F24 does not exist.
// Default: false (device must not stream outside Research mode); min/max:
// binary; ISettings key: none yet — SettingKey is append-only and empty
// (CR-003), so today the knob resolves from the F28_RESEARCH_CAPTURE_ENABLED
// build flag at compile time (§15 A3 flips it per capture build, repo
// default stays 0/false). Its future runtime home is F24's Research mode /
// a registered SettingKey; the runtime half of the gate is
// OperatingMode::Research (ResearchStreamer::active()).
constexpr bool kResearchCaptureDefaultEnabled = (F28_RESEARCH_CAPTURE_ENABLED != 0);

// --- line protocol ------------------------------------------------------------

// [DERIVED] F28 D10: version of the serial line + session schema, carried in
// #HDR and written to session.json (DATASET_FORMAT.md "Version history").
constexpr uint32_t kSessionSchemaVersion = 1;

// [DERIVED] Longest legal line. Worst case is an E row: 20-char TimeMs + 10-char
// seq/boot/duration/unix + 16-hex payload + the longest enum names + 16
// separators ≈ 220 B, so 256 leaves headroom (measured by
// test_event_worst_case_line_fits_max_line_len).
constexpr size_t kMaxLineLen = 256;

// [DERIVED] One tick of worst-case output: ≈15 raw rows at 50/100 Hz ≈ 700 B
// plus feature/event rows; 1024 B holds a full tick even if the UART stalls for
// one period (921600 drains it in ≈ 11 ms). Production pauses when only
// kLineRingReserveBytes remain, so a partial line never lands in the ring.
constexpr size_t kLineRingBytes = 1024;

// [DERIVED] Stop producing when fewer than one maximum line remains free —
// an append then always fits completely (F28 §10: drop/pause, never stall).
constexpr size_t kLineRingReserveBytes = kMaxLineLen;

// [HW] One bounded chunk per Serial write: 128 B is the ESP32 UART TX FIFO
// length, and arduino-esp32 installs NO TX ring by default (_txBufferSize(0),
// HardwareSerial.cpp), so Serial.availableForWrite() reports at most FIFO free
// space (esp32-hal-uart.c: uartAvailableForWrite -> uart_ll_get_txfifo_len).
// One drain per tick can therefore move at most kDrainChunkBytes — the chunk
// size is a hardware ceiling, only the tick rate can raise throughput. At
// 921600 the write shifts in ≈ 1.4 ms of UART time and never blocks (we only
// write what availableForWrite() already promised).
constexpr size_t kDrainChunkBytes = 128;

// --- rates and limits ----------------------------------------------------------

// [DERIVED] Research-task tick = mailbox poll + line format + UART drain
// cadence. Two requirements pin the value (F1 fixture root cause, 2026-10-13):
//  (1) drain throughput — capacity is one kDrainChunkBytes chunk per tick
//      ([HW] above). With the original 100 ms period the ceiling was
//      1280 B/s < 2275 B/s of 50 Hz I-row production: the line ring saturated
//      at t≈1.3 s, the reserve pause stopped tee draining and the tee
//      overflowed (12 201 raw drops in the 10-minute bench capture);
//  (2) poll rate — ≥10× faster than the 1000 ms feature rate (01 §3.3) so no
//      LatestValue is overwritten between two reads of ours; 10 ms is 100×.
// 10 ms ⇒ 12.8 KB/s of drain vs the 5.4 KB/s worst-case stream (2.4× headroom
// for interleaved diagnostics), ≈1.4 ms UART duty and ≈0.12 ms formatting per
// tick worst case (14 % / 1 % of the period).
constexpr uint32_t kResearchTaskPeriodMs = 10;

// [DERIVED] Sustained UART drain capacity = one kDrainChunkBytes chunk per
// task tick (exact here: 128 × 1000 / 10). Pinned to the worst-case load by
// the static_assert below — the F1 fixture's regression test.
constexpr uint32_t kDrainBytesPerSecond = kDrainChunkBytes * 1000u / kResearchTaskPeriodMs;

// [DERIVED] D2's worst-case M1 stream with the raw tee on: I 50 Hz × ≈45 B +
// P 100 Hz × ≈28 B + F/E/M ≈ 0.3 KB/s ≈ 5.4 KB/s. Sustaining less than this
// re-opens the ring → tee overflow path regardless of any other setting.
constexpr uint32_t kWorstCaseStreamBytesPerSecond = 5400;

// Root-cause guard (F1): a period regression must fail EVERY build loudly
// (same pattern as the A4 std::atomic static_assert), never silently re-open
// the drop path. Evidence: kResearchTaskPeriodMs back to 100 ⇒ 1280 < 5400 ⇒
// this line breaks `pio test -e native` and `pio run -e esp32dev`.
static_assert(kDrainBytesPerSecond >= kWorstCaseStreamBytesPerSecond,
              "research drain capacity below worst-case stream load: "
              "kResearchTaskPeriodMs too long for kDrainChunkBytes "
              "(F1 fixture: 1280 B/s ceiling dropped 12201 IMU samples / 10 min)");

// [DERIVED] Caps per-tick formatting work (40 lines × ≈3 µs ≈ 0.12 ms), so the
// research task can never spend its whole period formatting. Typical production
// is ≈17 lines/tick (50 Hz + 100 Hz + 1 Hz features).
constexpr size_t kMaxLinesPerTick = 40;

// [EXPERIMENTAL] Header repeat interval: a capture tool that attaches mid-stream
// learns schema/fw/drop counters within this window; tune with capture practice.
constexpr uint32_t kHeaderRepeatMs = 30'000;

// [DERIVED] Event burst cap per tick: 8 × ≈220 B ≈ 1.8 KB. The event cursor only
// advances when a row is actually emitted, so a burst beyond the ring delays
// rows to the next tick — it never loses them.
constexpr size_t kEventPollBatch = 8;

// [DERIVED] D8 baseline-drain cap: events already stored when a session starts
// are skipped by reading forward once at activation, at most this many copies
// (512 × 48 B ≈ 24 KB of reads, sub-millisecond in RAM, once per activation) so
// a pathologically full store cannot stall activation. A store larger than this
// may leak up to one batch of pre-existing rows into the session — recorded as
// an assumption (spec §13), not silently ignored.
constexpr size_t kEventBaselineMaxEvents = 512;

// [DERIVED] Firmware version field length in #HDR: longer values are truncated
// after sanitising (',' and newlines → '_'), keeping the field count stable for
// the parser (D6).
constexpr size_t kMaxFirmwareVersionLen = 32;

// --- raw tee (F28 D3) ----------------------------------------------------------

// [DERIVED] 16 × 32 B = 512 B = 320 ms of 50 Hz IMU ≫ the 10 ms poll period.
constexpr size_t kImuTeeDepth = 16;

// [DERIVED] 32 × 24 B = 768 B = 320 ms of 100 Hz PPG ≫ the 10 ms poll period.
constexpr size_t kPpgTeeDepth = 32;

// --- session identity -----------------------------------------------------------

// [DERIVED] No boot_id source exists in AppContext yet (F17 owns the counter
// and is not integrated); 0 therefore means "unknown", carried through to the
// session files and documented in DATASET_FORMAT.md.
constexpr uint32_t kUnknownBootId = 0;

}  // namespace bw::research
